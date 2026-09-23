/*
 * ===========================================================================
 *             ANALYSIS PIPELINE — IMPLEMENTATION
 * ===========================================================================
 * Source File: AnalysisPipeline.cpp
 *
 * TD-free. Contains the implementation of AnalysisPipeline that was previously
 * inlined in FFT.cpp. Moved here so the pipeline is unit-testable headlessly
 * (dsp_tests.cpp links this translation unit directly) without pulling in the
 * CHOP_CPlusPlusBase operator class from FFT.h.
 * ===========================================================================
 *
 * WHAT LIVES HERE, AND IN WHAT ORDER TO READ IT
 * ---------------------------------------------
 *   status()          - builds the telemetry struct; reads state, changes nothing
 *   rebuild()         - the "a setting that changes the transform changed" path:
 *                       FFT size, plan, pad offset, per-channel buffers
 *   updateWindow()    - regenerates the window coefficients when their key changes
 *   updateWarp()      - rebuilds the frequency-axis mapping tables (and, with them,
 *                       the reported axis rate - see the big comment above it)
 *   updateWeighting() - recomputes the equal-loudness curve when the axis or the
 *                       curve type changes
 *   runChannel()      - THE DSP CHAIN, one channel, one frame. If you only read one
 *                       function in this file, read this one; the six numbered steps
 *                       are the whole signal path.
 *   process()         - the entry point: caching decisions, the per-channel fan-out,
 *                       the peak telemetry
 *
 * The caching pattern used by the three update* functions is the same in each: build
 * a small key struct, compare it against the stored one, return early if it matches.
 * A table is therefore rebuilt when - and only when - an input it actually depends on
 * changed. See AnalysisPipeline.h for the rule about adding a parameter to a key.
 *
 * NOTHING HERE ALLOCATES IN THE STEADY STATE. process() pre-sizes every buffer that
 * runChannel() writes, because runChannel() is noexcept and runs off the cook thread.
 * If you add a stage, pre-size its output in process() too and follow the same rule.
 * ===========================================================================
 */

#include "AnalysisPipeline.h"
#include "RateModel.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <execution>
#include <numeric>
#include <string>

namespace {

using clk = std::chrono::steady_clock;
// Steady clock, not system clock: this measures elapsed time, and steady_clock is the one that
// cannot jump backwards if the machine's time is adjusted. Returns microseconds as a double.
inline double usSince(clk::time_point t0) { return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); }

// The TD-free sample-rate / bin / axis model (windowSamplesFrom, fftSizeFrom,
// outputBinCountFrom, axisRate, sampleRateToTouchDesigner, hzPerBin, throughput) lives in
// RateModel.h so it is shared by the CHOP and the pipeline and is unit-testable headlessly.
// (RateModel.h is included at global scope below / via FFT.h: its functions are all inline.)

} // namespace

// The Planner menu is cast straight onto FFTDSP::PlannerPolicy in rebuild(); pin the index alignment
// that cast depends on (it used to be "nothing asserts it").
static_assert(static_cast<int>(Parameters::Planner::Auto) == static_cast<int>(FFTDSP::PlannerPolicy::Auto) &&
              static_cast<int>(Parameters::Planner::Fast) == static_cast<int>(FFTDSP::PlannerPolicy::Fast) &&
              static_cast<int>(Parameters::Planner::Measured) == static_cast<int>(FFTDSP::PlannerPolicy::Measured) &&
              static_cast<int>(Parameters::Planner::Patient) == static_cast<int>(FFTDSP::PlannerPolicy::Patient) &&
              static_cast<int>(Parameters::Planner::COUNT) == 4,
              "Parameters::Planner and FFTDSP::PlannerPolicy must keep the same numbering");
static_assert(static_cast<int>(Parameters::WarpAggregate::Off) == 0 && static_cast<int>(Parameters::WarpAggregate::Peak) == 1 &&
              static_cast<int>(Parameters::WarpAggregate::Rms) == 2,
              "WarpAggregate values are PerceptualWarping::setAggregation codes");

// =============================================================================================
// AnalysisPipeline
// =============================================================================================
// The constructor is deliberately trivial: it stores the log pointer and nothing else. Every table
// and every buffer is built lazily, on the first process() call, because the sizes and the axis
// settings come from the first job the node hands over - and the engine must not plan an FFT at a
// size the parameters are about to change.
AnalysisPipeline::AnalysisPipeline(FFTDSP::PlanLog* log)
	: myLog(log)
{
}

// The engine (a unique_ptr) destroys its plan here; see FFTWEngine::~FFTWEngine, which joins the
// background planner thread before tearing the plan down. Nothing else needs explicit cleanup.
AnalysisPipeline::~AnalysisPipeline()
{
}

// Telemetry snapshot for the Info CHOP and the Info DAT. Reads only; changes nothing, and is cheap
// (it copies strings from state that is already built). Every field is read back off what the
// pipeline actually built - the warp tables in particular - rather than recomputed from the
// parameters, so a display of this can never disagree with the audio path.
AnalysisPipeline::Status
AnalysisPipeline::status() const
{
	Status s;
	s.plan = myEngine ? myEngine->getPlanStatus() : std::string("Uninitialized");
	s.backend = myEngine ? myEngine->backendReport() : std::string();
	s.planFailed = planFailed();
	s.fftSize = myFFTSize;
	s.capacity = myCapacity;
	s.linearBins = myFFTSize / 2 + 1;   // the real-to-complex bin count of the FFT itself
	s.magnitudeBins = myMagnitudeBins;
	s.axisRate = myOutputSampleRate;
	// The grid's actual low end. Not always DC: only Mel, ERB and Linear start there, and only at
	// blend 0 is every scale pulled down to it — Log starts at Log Floor, Chroma at 20 Hz, Bark at
	// ~13 Hz. Reported so the axis row cannot claim 0 Hz for a grid that does not reach it.
	{
		const std::vector<double>& hz = myWarping.targetHz();
		s.axisBottom = hz.empty() ? 0.0 : hz.front();
	}
	s.outputBins = static_cast<int>(myWarping.outputBins());
	s.linearGrid = myLinearGrid;
	s.planUpgrading = myEngine ? myEngine->upgradeInProgress() : false;
	s.kaiserBeta = myKaiserBeta;
	s.aggregation = myWarping.aggregation();
	s.aggregatedBins = myWarping.aggregatedBins();
	s.abandonedMeasurements = myEngine ? myEngine->abandonedMeasurements() : 0;
	return s;
}

/*
rebuild() - everything that has to change when a setting that defines the transform changes:
the FFT size, the plan behind it, the zero-pad offset, and every per-channel buffer.

WHEN IT RUNS: only from process(), and only when the rebuild_needed predicate there is true - i.e.
when the input sample rate, the window length, the pad size, the planner policy or the backend
changed. Everything else (axis, bins, weighting, ballistics) is handled by the three update*
functions, which are cheap enough to check every cook.

WHY prepare() IS CALLED UNCONDITIONALLY HERE: the engine's prepare() has its own early-out that
compares size, policy and backend, so calling it every rebuild costs one comparison and keeps the
"what does the plan depend on" question in exactly one place. Do not add a condition here.

THE PER-CHANNEL BUFFERS are re-assigned (not merely resized) on purpose: `assign(size, 0.0f)`
zeroes the padded frame, which is what the FFT needs for the unused part of the frame after a size
change. `prev_spectrum.clear()` drops the tracked history - after a rebuild the old smoothed
frame belongs to a different axis or a different size, so carrying it forward would show a
transient that no input caused. `agc_peak` is reset for the same reason.
*/
void
AnalysisPipeline::rebuild(const AnalysisJob& job)
{
	mySampleRate = job.sampleRate;
	myCapacity = std::max<size_t>(1, static_cast<size_t>(job.winSamples));   // never 0: the window math below divides by nothing, but the FFT of a 0-length frame would be meaningless
	myPadChoice = job.p.zeroPad ? job.p.padSize : 0;   // 0 = Zero-Padding off (the transform is the window)
	myPlanner = static_cast<FFTDSP::PlannerPolicy>(job.p.planner);
	// Parameters::Backend and FFTDSP::backendById() are the same numbering by construction (pinned by
	// the static_asserts after the menu tables in Parameters.cpp, which compare the enum against
	// FFTDSP::backendCount() and against the per-backend entries themselves); the cast keeps the
	// dependency one-way, pipeline -> DSP.
	myBackend.store(&FFTDSP::backendById(static_cast<int>(job.p.backend)), std::memory_order_release);

	// FFT size >= zero-pad length and >= next power of two of the window (window never overflows the frame)
	myFFTSize = fftSizeFrom(job.p, static_cast<int>(myCapacity));

	// 8-float aligned centre offset: a shift of < 8 samples only changes phase, never magnitude.
	// Zero-padding a windowed block and centring it is the standard way to keep a transient away
	// from the frame edges; the exact offset only affects the phase of the result, which nothing
	// downstream looks at, so it is rounded down to a multiple of 8 to keep the FFT's input pointer
	// 32-byte aligned (and therefore eligible for the aligned SIMD loads).
	size_t centre = (myFFTSize > myCapacity) ? (myFFTSize - myCapacity) / 2 : 0;
	myPadStart = centre & ~static_cast<size_t>(7);

	// Force the window and warp tables to rebuild: they are sized from the values set above, so no
	// key from before this point can be trusted. updateWindow/updateWarp run later in process()
	// and see these reset keys as "changed".
	myWindowKey = WindowKey{};
	myWarpKey = WarpKey{};

	if (!myEngine) myEngine = std::make_unique<FFTDSP::FFTWEngine>();
	myEngine->prepare(myFFTSize, myPlanner, myLog, myBackend.load(std::memory_order_relaxed));

	// linearBinCount() is the FFT's own bin count (N/2+1), not the output bin count: these buffers
	// hold the raw linear spectrum, before the warp reshapes it onto the output axis.
	for (auto& ch : myChannels) {
		ch.padded_frame.assign(myFFTSize, 0.0f);
		ch.rfft_magnitude.assign(FFTDSP::PerceptualWarping::linearBinCount(myFFTSize), 0.0f);
		ch.scratch_complex.resize(FFTDSP::PerceptualWarping::linearBinCount(myFFTSize));
		ch.prev_spectrum.clear();
		ch.prev_linear.clear();
		ch.agc_peak = 0.0f;
	}
	// The Info DAT shows a plan description and an FFT size; both may have just changed, so the
	// telemetry version is bumped to tell the cook thread's memoization to re-read status().
	++myStatusVersion;
}

// Regenerates the window coefficients when the window type, its Kaiser beta, the window length or
// the magnitude normalization changed. Called every cook; returns immediately in the steady state.
//
// The second half of the guard (myWindowBuffer.size() != myCapacity) covers the one case the key
// cannot: the very first call, and any path that emptied the buffer while leaving the key intact.
void
AnalysisPipeline::updateWindow(const Parameters::Values& p)
{
	// The beta that is actually used: Kaiser Beta Mode = Auto derives it from the dB range (RateModel.h),
	// so the key holds the effective value and a dB-range change regenerates the window by itself.
	const double beta = effectiveKaiserBeta(p);
	WindowKey key{ static_cast<int>(p.window), beta, myCapacity, static_cast<int>(p.magNorm) };
	if (key == myWindowKey && myWindowBuffer.size() == myCapacity) return;
	myKaiserBeta = (p.window == Parameters::WindowType::Kaiser) ? beta : 0.0;
	++myStatusVersion;
	// The normalization mode is translated from the parameter enum to the DSP enum here, which is
	// the only place the two meet: CoherentGain keeps the historical N_win/2 reading that existing
	// projects' dB offsets were tuned against; FullScale makes a full-scale sine read its own
	// amplitude. See WindowGenerator in DSPModules.h for what each one does to the numbers.
	FFTDSP::WindowGenerator::generateWindow(static_cast<int>(p.window), beta, myCapacity, myWindowBuffer,
	                                        p.magNorm == Parameters::MagNorm::FullScale ? FFTDSP::WindowNorm::FullScale
	                                                                                    : FFTDSP::WindowNorm::CoherentGain);
	myWindowKey = key;
}

/*
updateWarp() - rebuilds the mapping tables that turn the FFT's uniform linear bins into the
output axis (uniform, logarithmic, or one of the perceptual scales), and derives from them the
two numbers the rest of the node depends on: which linear bins must actually be computed, and
what sample rate the CHOP should report.

The block below explains the rate model in full. The short version: the output axis always ends
exactly on fmax, so the reported axis rate is exactly 2*fmax and never needs to be fitted.

NOTE THE ORDERING: myWarpVersion is bumped at the end, and updateWeighting() uses that counter as
part of its key. That is how a change of axis - which moves every bin's frequency and therefore
invalidates the weighting curve - propagates without anyone having to remember to recompute the
curve explicitly.

Called every cook; returns immediately in the steady state (the key comparison is 8 scalars).

Output rate model — what "sample rate" means for a spectrum.

A CHOP sample rate normally says how many time samples fit in a second. For a spectrum the
coherent reading is the band the axis covers: bin 0 is DC, the last bin is the top of the
displayed band, and the axis spans 0..sr_out/2 — i.e. the last bin sits on Nyquist. So the
rate is not the input rate and not something to be fitted, it is simply twice the top of the
axis the warp actually built:

    sr_out = 2 * target_hz[n_out-1] = 2 * fmax

Every grid this node can emit is covered by that one line:

  linear grid, fmax == nyquist       fmax is the top of the band, so sr_out = sr_in exactly. When
                                     Output Bins also equals fft_size/2+1 the warp is the identity
                                     and the bins are the untouched FFT bins, making the implied
                                     spacing sr_out/(2*(n_out-1)) exactly sr_in/fft_size — the
                                     transform's own resolution. (This is the setting that used to be
                                     the "Raw Linear Bins" toggle; it was removed because these
                                     sliders already produce it.)
  warped grid, Display Max < nyquist the axis stops at Display Max instead of Nyquist, so
                                     sr_out = 2*Display Max. Keeping sr_in there would claim
                                     the spectrum reaches sr_in/2 Hz when it really stops at
                                     Display Max — the error the old code made.
  warped grid, fmax == nyquist       sr_out = sr_in: same band as the input, only resampled
                                     onto a different number of bins. Resampling changes how
                                     many bins describe the band, never how wide the band is,
                                     so the rate correctly does not move with Output Bins.
  perceptual (Log/Mel/ERB/...)       non-uniform bin spacing, but the TOP endpoint is still fmax
                                     for every scale, so sr_out = 2*fmax still holds and is exact
                                     at the top; only the bins in between are non-uniform, which no
                                     single rate could describe anyway. (The bottom endpoint is
                                     blend * perceptual[0], so it is exactly DC only when the blend
                                     is 0 or the scale itself starts at 0 Hz — Mel, ERB, Linear. Log
                                     starts at Log Floor, Chroma at 20 Hz, Bark at ~13 Hz. That is a
                                     property of the scale, not of the rate model: the model only
                                     claims the top of the axis.)

Note that the old `Output Bins`-dependent form (2*fmax*bins/(bins-1)) was wrong: it stretched
the axis by one bin, putting Nyquist one bin above Display Max.
*/
void
AnalysisPipeline::updateWarp(const Parameters::Values& p)
{
	const size_t n_linear_bins = FFTDSP::PerceptualWarping::linearBinCount(myFFTSize);
	const double nyquist = mySampleRate / 2.0;
	// Raw RFFT Bins: a Linear DC..Nyquist grid with exactly one output bin per rfft bin - the identity,
	// which buildWarpTables detects and applyWarp runs as a memcpy (the rfft magnitude, untouched).
	const bool   raw = p.rawBins;
	// Display Max is clamped to Nyquist: above it there are no bins to show, and a larger axis would
	// just leave the top of the output empty and the bin spacing wrong.
	const double fmax = raw ? nyquist : std::min(p.displayMax, nyquist);
	const int    scale = raw ? static_cast<int>(Parameters::Scale::Linear) : static_cast<int>(p.scale);
	const double blend = raw ? 0.0 : p.warp;
	// Auto / Raw: the transform's own N/2+1, read off the FFT that was built (not recomputed), so the
	// count can never disagree with the magnitude buffer it is copied from.
	const size_t n_out = (raw || p.binsMode == Parameters::BinsMode::Auto) ? n_linear_bins
	                                                                        : static_cast<size_t>(outputBinCountFrom(p, mySampleRate));
	const int    interp = raw ? 0 : static_cast<int>(p.warpInterp);
	const int    agg = raw ? 0 : static_cast<int>(p.warpAggregate);

	WarpKey key{ scale, fmax, static_cast<int>(n_out), blend, p.logFloor, n_linear_bins, nyquist, interp, agg };
	if (key == myWarpKey) return;
	// See the note on WarpKey in AnalysisPipeline.h: this is the only call site of
	// setInterpolation / setAggregation, so both modes have to be part of the key above.
	myWarping.setInterpolation(interp);
	myWarping.setAggregation(agg);
	myWarping.buildWarpTables(scale, fmax, n_out, nyquist, blend, p.logFloor, n_linear_bins);
	// How much of the linear spectrum is worth computing. The warp only ever reads up to
	// maxLinearIndex(), so bins above it would be transform and magnitude work whose result is
	// discarded - which is the main saving of a narrow Display Max.
	myMagnitudeBins = std::min(n_linear_bins, myWarping.maxLinearIndex() + 1);
	// Top of the axis is fmax by construction (both endpoints are pinned in computeTargetHzGrid),
	// so the axis rate follows from fmax alone. Published for the cook thread, which reads it back
	// through outputAxisRate(). When fmax has been clamped to Nyquist the axis rate is sr_in. Note
	// this is NOT info->sampleRate — that is bins x me.time.rate (see outputSampleRate()).
	myOutputSampleRate = 2.0 * fmax;
	// Read the identity back off the tables rather than inferring it from the parameter values: this
	// is the same flag applyWarp() branches on, so the Info CHOP cannot claim a bypass that the
	// pointer loop below did not take.
	myLinearGrid = myWarping.isIdentity();
	myWarpKey = key;
	++myWarpVersion;
	++myStatusVersion;
}

// Recomputes the equal-loudness weighting curve when the curve type changed or the axis it is
// sampled against changed. The axis is the reason this cannot be a constant table: the curve is
// per *bin*, and on a warped grid the bins are not evenly spaced in Hz.
//
// The key carries no geometry of its own - the warp version counter covers all of it, and it
// changes whenever the bins do. The extra size check is the same first-call guard as in
// updateWindow.
void
AnalysisPipeline::updateWeighting(const Parameters::Values& p)
{
	WeightKey key{ static_cast<int>(p.weighting), myWarpVersion };
	if (key == myWeightKey && myWeightingCurve.size() == myWarping.outputBins()) return;
	FFTDSP::EqualLoudness::computeCurve(static_cast<int>(p.weighting), myWarping.targetHz(), myWeightingCurve);
	myWeightKey = key;
}

/*
runChannel() - THE DSP CHAIN. One channel, one frame, start to finish.

THE SIX STEPS, in the order they appear below and the order they must happen in:
  1. window        - multiply the frame by the window coefficients, into the padded buffer
  2. FFT+magnitude - run the plan, then |X| for the bins the warp will read
  2b. normalize    - full-scale correction for the two bins with no mirror partner
  3. warp          - resample the linear bins onto the output axis (memcpy when they coincide)
  4. weighting     - multiply by the equal-loudness curve
  5. dB            - convert to the selected unit, using the selected reference
  6. ballistics    - smooth across frames

WHY THE ORDER IS WHAT IT IS: the window has to come before the FFT and the magnitude after it, and
the warp has to come before the weighting (the curve is sampled on the output axis, not the linear
one). dB must come after the weighting, since weighting in dB would be an addition, and ballistics
must come last because it is the only stage that carries state between frames - smoothing a value
that a later stage is about to rescale would make the time constants mean nothing.

`out` is the caller's output spectrum, pre-sized to the output bin count by process(). This
function never resizes it (see the assert) and never allocates, because it is noexcept and runs on
a worker thread.

`silent` is the caller's "this whole window is digital silence" flag, used only for the cheap
short-circuit at the top; see that comment for why it does not always apply.
*/
void
AnalysisPipeline::runChannel(DspState& st, const FFTDSP::AlignedVector& window_in, bool silent, const Parameters::Values& p,
                             float attackCoef, float releaseCoef, float agcDecay, FFTDSP::AlignedVector& out) noexcept
{
	const size_t bins = myWarping.outputBins();
	assert(out.size() == bins);   // process() pre-sizes spectra; a size mismatch here is an invariant violation, not an allocation

	// Digital silence: nothing to analyse. (dB modes need the floor value and ballistics need to decay,
	// so the short-circuit only applies to the plain linear-magnitude path.)
	// The condition is the whole story: in linear-magnitude mode with no smoothing, silence must
	// produce silence, and skipping four stages of work for it is free. In any other configuration
	// the output of a silent frame is NOT zero - a dB mode has to publish the floor, and a ballistic
	// filter has to keep decaying toward it - so the full chain runs.
	if (silent && p.loudness == Parameters::Loudness::Off && !p.ballEnable) {
		std::memset(out.data(), 0, bins * sizeof(float));
		st.prev_spectrum.clear();   // no history worth carrying: there is nothing to smooth into
		return;
	}

	// 1. window into the aligned centre of the persistent zero-padded frame
	// The frame is persistent (one per channel, resized only in rebuild()) and only the windowed
	// region is written here; the rest stays zero, which is what makes this a zero-padded transform
	// without a memset per cook. `win_len` is the min of the three lengths so a mismatch between the
	// window buffer, the job's samples and the capacity cannot overrun - it can only shorten the
	// window, and the guard below then leaves the frame untouched rather than writing past it.
	size_t win_len = std::min({ myCapacity, myWindowBuffer.size(), window_in.size() });
	if (st.padded_frame.size() >= myPadStart + win_len) {
		FFTDSP::multiplyInto(window_in.data(), myWindowBuffer.data(), st.padded_frame.data() + myPadStart, win_len);
	}

	// 2. FFT + magnitude (only the bins the warp will read)
	myEngine->executeRFFT(st.padded_frame, st.rfft_magnitude, st.scratch_complex, myMagnitudeBins);

	// 2b. full-scale normalization: DC / Nyquist have no mirror bin
	// A real-to-complex transform folds the negative frequencies onto the positive ones, so every
	// interior bin already counts its mirror partner and DC and Nyquist do not. Halving them makes
	// the whole spectrum mean the same thing (a full-scale sine reads its own amplitude) - which is
	// exactly what the FullScale normalization mode promises. In CoherentGain mode this is not done,
	// because that mode's historical reading is the one existing projects tuned their offsets against.
	if (p.magNorm == Parameters::MagNorm::FullScale && st.rfft_magnitude.size() >= 2) {
		st.rfft_magnitude.front() *= 0.5f;
		st.rfft_magnitude.back() *= 0.5f;
	}

	// 3. psychoacoustic warp (linear or cubic; identity memcpy when 1:1)
	// Reads st.rfft_magnitude, writes out. `out` is the caller's buffer, and it is the only stage
	// that both reads and writes a spectrum this way - which is why the identity case is a memcpy
	// there rather than an in-place no-op.
	myWarping.applyWarp(st.rfft_magnitude, out);

	// 4. equal-loudness weighting
	// The size check guards the one case the weighting key cannot see coming: a curve built for a
	// different bin count (the axis is rebuilt before this runs, but the curve's own update happens
	// in process(), and a mismatch here must skip rather than read past the curve).
	if (p.weighting != Parameters::Weighting::Off && myWeightingCurve.size() == out.size()) {
		FFTDSP::multiplyInPlace(out.data(), myWeightingCurve.data(), out.size());
	}

	// 5. dB with the selected reference
	const int loudness = static_cast<int>(p.loudness);
	if (loudness != 0) {
		// st.prev_spectrum holds the previous frame in whatever unit that frame was in. Crossing the
		// Off boundary changes the unit (linear magnitude <-> dB), and smoothing across a unit change
		// would drag a dB frame toward a linear magnitude, so the history is dropped at the crossing.
		// Only the boundary is handled: dB <-> dB-normalized is a rescale of the same dB curve and is
		// continuous enough to keep smoothing through.
		if (st.prev_loudness_mode == 0) st.prev_spectrum.clear();
		float peak = FFTDSP::peakMagnitude(out.data(), out.size());
		float ref = 1.0f;
		// The three dB references, and what each one means physically:
		//   Dbfs      - 0 dB is the loudest a full-scale signal could be, so the reading is absolute.
		//               The reference is the window's own coherent gain (N/2) so that a full-scale sine
		//               reads ~0 dB rather than a number that depends on the FFT size; in FullScale
		//               normalization mode the magnitudes are already absolute, so the reference is 1.
		//   Agc       - 0 dB is the recent loudest bin of this channel, tracked per channel with an
		//               asymmetric follower (instant attack, agcDecay release). The display then shows
		//               a spectrum that is always "full" whatever the input level is.
		//   FramePeak - 0 dB is this frame's own peak. Per-frame, so it does not track loudness at all;
		//               useful for seeing the shape of a spectrum with the level normalized out.
		switch (p.dbRef) {
			case Parameters::DbRef::Dbfs:
				ref = (p.magNorm == Parameters::MagNorm::FullScale) ? 1.0f : static_cast<float>(myCapacity) * 0.5f;
				break;
			case Parameters::DbRef::Agc:
				// max(peak, decayed follower): attack is instant because the max takes the peak the
				// moment it appears; only the release is smoothed, which is what keeps the display
				// from pumping on a signal whose level is dropping.
				st.agc_peak = std::max(peak, st.agc_peak * agcDecay);
				ref = st.agc_peak;
				break;
			case Parameters::DbRef::FramePeak:
			default:
				ref = peak;
				break;
		}
		// A silent frame has a peak of 0, and 1/0 would make the converter's input infinite. Falling
		// back to 1.0 puts the frame at the dB floor instead, which is the correct picture of silence.
		if (!(ref > 0.0f)) ref = 1.0f;
		// The converter takes 1/ref (a multiply per bin) rather than dividing per bin.
		FFTDSP::DecibelConverter::convertToDB(loudness, p.dbRange, 1.0f / ref, out);
	} else if (st.prev_loudness_mode != 0) {
		// Leaving a dB mode for linear: the stored history is in dB, so it cannot be smoothed against
		// a linear frame. (The mirror of the check above, for the same reason.)
		st.prev_spectrum.clear();
	}
	st.prev_loudness_mode = loudness;

	// 6. ballistics (bypassed at 0/0)
	if (attackCoef > 0.0f || releaseCoef > 0.0f) {
		FFTDSP::BallisticsFilter ball;
		// apply() reads `current` and writes the smoothed frame into prev_out; it never writes through
		// `current`. So the smoothed result has to be copied back into the buffer the caller publishes,
		// and prev_spectrum keeps only the history the next frame smooths against.
		// The filter object is constructed per call and holds no state itself - all the state is in
		// prev_spectrum, which is why one stack object per channel per frame is correct and cheap.
		ball.apply(attackCoef, releaseCoef, out, st.prev_spectrum);
		std::memcpy(out.data(), st.prev_spectrum.data(), bins * sizeof(float));
	}
}

/*
process() - THE ENTRY POINT. One call analyses one cooked frame for every channel.

WHAT IT DOES, in order:
  1. decide whether the transform itself has to be rebuilt, and rebuild it if so
  2. hand the Async toggle to the engine, then collect any finished background plan
  3. keep the per-channel state in step with the channel count, and honour a reset request
  4. refresh the three cached tables (window, warp, weighting) - all cheap no-ops in the steady state
  5. turn the ballistics parameters into this frame's coefficients
  6. run the per-channel chain, in parallel when there is more than one channel
  7. measure the channel-0 peak for the Info CHOP

WHERE THIS IS CALLED FROM: FFT::runJob in FFT.cpp - on the analysis worker thread when Async is on,
on the cook thread when it is off. It is never called concurrently with itself.
*/
void
AnalysisPipeline::process(const AnalysisJob& job, AnalysisResult& res)
{
	const auto t0 = clk::now();
	const Parameters::Values& p = job.p;

	// The rebuild predicate: exactly the settings that define the transform's shape. Everything else
	// is handled by the update* functions below, which are cheap enough to check every cook - this
	// list exists because a rebuild destroys and re-creates the FFT plan, which is genuinely
	// expensive, so it must not happen for a change that only moves bins around.
	//
	// A backend switch is in the list for a subtler reason than the others: a plan belongs to the
	// library that made it, so the old plan has to be destroyed by its own fftwf_destroy_plan before
	// the new one replaces it. prepare() does that; skipping the rebuild would hand one library's
	// plan to the other.
	const bool rebuild_needed = !myEngine
	                         || std::abs(job.sampleRate - mySampleRate) > 1e-3
	                         || static_cast<size_t>(job.winSamples) != myCapacity
	                         || (p.zeroPad ? p.padSize : 0) != myPadChoice
	                         || static_cast<FFTDSP::PlannerPolicy>(p.planner) != myPlanner
	                         // A backend switch must rebuild: the plan object belongs to the library
	                         // that made it, so the old plan has to be destroyed by its own
	                         // fftwf_destroy_plan before the new one replaces it. prepare() does that.
	                         || &FFTDSP::backendById(static_cast<int>(p.backend)) != myBackend.load(std::memory_order_relaxed);
	if (rebuild_needed) rebuild(job);
	// The Async toggle, handed to the engine every cook before it is polled. Async off must mean one
	// thread for the whole node - the cook thread - and the FFTW planner's deferred MEASURE/PATIENT
	// upgrade is the one piece of work that would otherwise still run off it. The engine decides what
	// that means for a plan it already holds (see FFTWEngine::setBackgroundAllowed); this call is
	// deliberately unconditional and cheap, so a flip is never missed by the rebuild early-out.
	if (myEngine) myEngine->setBackgroundAllowed(p.async);
	// A swapped-in plan changes the plan description and ends the "upgrading" state, so both bump
	// the status version below.
	if (myEngine->pollBackgroundPlan()) ++myStatusVersion;
	const bool upgrading = myEngine->upgradeInProgress();
	if (upgrading != myLastUpgrading) { myLastUpgrading = upgrading; ++myStatusVersion; }

	// Grow the per-channel state to match the job. Only the NEW entries are initialised: a channel
	// that already exists keeps its padded frame, its magnitude buffer and - importantly - its
	// smoothed history and AGC follower, which is what makes a channel's display continuous across
	// cooks. Shrinking simply drops the extra entries (their history is deliberately not preserved
	// for a channel that goes away and comes back).
	if (myChannels.size() != static_cast<size_t>(job.numChannels)) {
		size_t old = myChannels.size();
		myChannels.resize(static_cast<size_t>(job.numChannels));
		for (size_t i = old; i < myChannels.size(); ++i) {
			myChannels[i].padded_frame.assign(myFFTSize, 0.0f);
			myChannels[i].rfft_magnitude.assign(FFTDSP::PerceptualWarping::linearBinCount(myFFTSize), 0.0f);
			myChannels[i].scratch_complex.resize(FFTDSP::PerceptualWarping::linearBinCount(myFFTSize));
		}
	}
	// A reset (the node's Reset pulse, or a fresh start) means "forget the past": drop the smoothed
	// history and the AGC follower so the display restarts clean instead of decaying from a value
	// that belongs to the previous session.
	if (job.reset) {
		for (auto& ch : myChannels) { ch.prev_spectrum.clear(); ch.prev_linear.clear(); ch.agc_peak = 0.0f; }
	}

	// The three cached tables, in dependency order: the warp tables define the axis, and the
	// weighting curve is sampled on that axis, so the warp must be updated first.
	updateWindow(p);
	updateWarp(p);
	updateWeighting(p);

	// Ballistics coefficients, converted once per cook (not per channel) because they depend only on
	// the frame delta. The two modes are deliberately different things: Milliseconds converts a
	// time constant into a frame-rate-independent coefficient, while the raw attack/release mode
	// passes the user's numbers straight through as per-frame coefficients, which is what the older
	// projects were built against. agcDecay is the AGC follower's release, fixed at 1500 ms.
	float attackCoef = 0.0f, releaseCoef = 0.0f;
	if (p.ballEnable) {
		if (p.ballMode == Parameters::BallisticsMode::Milliseconds) {
			attackCoef  = FFTDSP::BallisticsFilter::coefFromMs(p.attackMs, job.dtMs);
			releaseCoef = FFTDSP::BallisticsFilter::coefFromMs(p.releaseMs, job.dtMs);
		} else {
			attackCoef  = static_cast<float>(p.attack);
			releaseCoef = static_cast<float>(p.release);
		}
	}
	const float agcDecay = FFTDSP::BallisticsFilter::coefFromMs(1500.0, job.dtMs);

	std::vector<FFTDSP::AlignedVector>& out = res.spectra;
	// Size the result vector to the channel count, then pre-size every spectrum in it. Both are
	// no-ops once the channel count and the output bin count have settled, which is the point:
	// see the comment below.
	if (out.size() != static_cast<size_t>(job.numChannels)) out.resize(static_cast<size_t>(job.numChannels));
	// Pre-size every output spectrum here (not in the noexcept runChannel): with output bins stable
	// across cooks, steady state performs zero allocations on the cook path, so a bad_alloc can no
	// longer terminate a noexcept function. runChannel asserts the invariant instead of resizing.
	const size_t bins = myWarping.outputBins();
	for (auto& s : out) {
		if (s.size() != bins) s.resize(bins);
	}

	// 2. The whole per-channel pipeline, one channel per DspState. Channels share nothing but
	// read-only tables (window, warp tables, weighting curve) and the FFT plan, so the loop is
	// embarrassingly parallel and runs that way whenever there is more than one channel.
	//
	// This node is one mono channel per instance — several channels means several nodes, each with
	// its own analysis worker — so in normal use numChannels is 1 and none of this runs. It is here
	// for the one mode that still produces several transforms per cook, Channels = All Channels.
	// There is no parameter: with one channel the branch is a single integer compare and the serial
	// path is taken, and with several, serial would be measurably worse for nothing.
	//
	// Threads do not help a *single* transform: FFTW cannot split a single 1-D transform: measured, nthreads > 1
	// makes one transform 13-51 % slower, not faster, which is why the parallelism has to be the
	// channel loop or nothing.
	myParallelActive.store(job.numChannels > 1, std::memory_order_relaxed);
	if (job.numChannels > 1) {
		// The index vector exists only to give std::for_each something to iterate in parallel; it is
		// built here rather than kept as a member because it is small and this path is not hot (the
		// hot path is numChannels == 1, which skips all of this). A future optimization could hoist
		// it into a member that is grown and never shrunk - it changes no results.
		// The index list is a member grown to the channel count once, not a vector per job: the
		// steady-state "no allocation" invariant covers All Channels too (pinned by the allocation gate
		// in tests/dsp_tests.cpp).
		if (myChannelIndex.size() != static_cast<size_t>(job.numChannels)) {
			myChannelIndex.resize(static_cast<size_t>(job.numChannels));
			std::iota(myChannelIndex.begin(), myChannelIndex.end(), 0);
		}
		std::for_each(std::execution::par, myChannelIndex.begin(), myChannelIndex.end(), [&](int ch) {
			// A channel may be missing its silent flag (silent.size() is allowed to be shorter than
			// the channel count); the bounds check treats that as "not silent", which is the safe
			// default - the full chain runs and produces a real spectrum.
			const bool silent = ch < static_cast<int>(job.silent.size()) && job.silent[ch] != 0;
			runChannel(myChannels[static_cast<size_t>(ch)], job.windows[static_cast<size_t>(ch)], silent, p,
			           attackCoef, releaseCoef, agcDecay, out[static_cast<size_t>(ch)]);
		});
	} else {
		// The serial path for the single-channel case. Deliberately the same call as the lambda above
		// so the two cannot drift apart.
		for (int ch = 0; ch < job.numChannels; ++ch) {
			const bool silent = ch < static_cast<int>(job.silent.size()) && job.silent[ch] != 0;
			runChannel(myChannels[static_cast<size_t>(ch)], job.windows[static_cast<size_t>(ch)], silent, p,
			           attackCoef, releaseCoef, agcDecay, out[static_cast<size_t>(ch)]);
		}
	}

	// Peak telemetry (channel 0) belongs here, not on the cook thread: the owner has the Hz table.
	// Only channel 0 is measured - the Info CHOP reports one peak, and doing this per channel would
	// add a pass over every spectrum for a number nobody reads.
	// Spectral features (channel 0, optional): from the LINEAR magnitude, so they do not depend on the
	// display axis. A silent channel short-circuits runChannel and leaves its magnitude stale, so it
	// reports the silence values instead of reading it.
	res.hasFeatures = p.features && job.numChannels > 0 && !myChannels.empty();
	if (res.hasFeatures) {
		const bool silent0 = !job.silent.empty() && job.silent[0] != 0;
		DspState& c0 = myChannels[0];
		const size_t n = std::min(myMagnitudeBins, c0.rfft_magnitude.size());
		if (silent0 || n == 0) {
			res.features = FFTDSP::SpectralFeatures{};
			c0.prev_linear.clear();
		} else {
			const double binHz = myFFTSize ? mySampleRate / static_cast<double>(myFFTSize) : 0.0;
			const FFTDSP::AlignedVector& w0 = job.windows[0];
			FFTDSP::computeSpectralFeatures(c0.rfft_magnitude.data(), n, binHz, w0.data(),
			                                std::min(w0.size(), myCapacity), c0.prev_linear, res.features);
		}
	}

	res.peakMag = 0.0f;
	res.peakHz = 0.0f;
	if (!out.empty() && !out[0].empty()) {
		size_t max_idx = 0;
		res.peakMag = FFTDSP::findPeakWithIndex(out[0].data(), out[0].size(), max_idx);
		// targetHz() is the literal output grid, whatever built it, so the peak's Hz needs no special
		// case — not even for the identity grid, where the table is exactly index * sr/fft_size.
		// The bounds check is a guard, not a case: the table is built for exactly outputBins()
		// entries and max_idx indexes the spectrum, which is that same length.
		const std::vector<double>& hz = myWarping.targetHz();
		if (max_idx < hz.size()) res.peakHz = static_cast<float>(hz[max_idx]);
	}
	myLastUs = usSince(t0);
}