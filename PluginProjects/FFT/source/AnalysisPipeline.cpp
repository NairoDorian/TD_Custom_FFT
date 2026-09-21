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
 */

#include "AnalysisPipeline.h"

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
inline double usSince(clk::time_point t0) { return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); }

// The TD-free sample-rate / bin / axis model (windowSamplesFrom, fftSizeFrom,
// outputBinCountFrom, axisRate, sampleRateToTouchDesigner, hzPerBin, throughput) lives in
// RateModel.h so it is shared by the CHOP and the pipeline and is unit-testable headlessly.
#include "RateModel.h"

} // namespace

// =============================================================================================
// AnalysisPipeline
// =============================================================================================
AnalysisPipeline::AnalysisPipeline(FFTDSP::PlanLog* log)
	: myLog(log)
{
}

AnalysisPipeline::~AnalysisPipeline()
{
}

AnalysisPipeline::Status
AnalysisPipeline::status() const
{
	Status s;
	s.plan = myEngine ? myEngine->getPlanStatus() : std::string("Uninitialized");
	s.backend = myEngine ? myEngine->backendReport() : std::string();
	s.planFailed = planFailed();
	s.fftSize = myFFTSize;
	s.capacity = myCapacity;
	s.linearBins = myFFTSize / 2 + 1;
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
	return s;
}

void
AnalysisPipeline::rebuild(const AnalysisJob& job)
{
	mySampleRate = job.sampleRate;
	myCapacity = std::max<size_t>(1, static_cast<size_t>(job.winSamples));
	myPadChoice = job.p.padSize;
	myPlanner = static_cast<FFTDSP::PlannerPolicy>(job.p.planner);
	// Parameters::Backend and FFTDSP::backendById() are the same numbering by construction (see the
	// static_assert in FFT.cpp); the cast keeps the dependency one-way, pipeline -> DSP.
	myBackend = &FFTDSP::backendById(static_cast<int>(job.p.backend));

	// FFT size >= zero-pad length and >= next power of two of the window (window never overflows the frame)
	myFFTSize = fftSizeFrom(job.p, static_cast<int>(myCapacity));

	// 8-float aligned centre offset: a shift of < 8 samples only changes phase, never magnitude
	size_t centre = (myFFTSize > myCapacity) ? (myFFTSize - myCapacity) / 2 : 0;
	myPadStart = centre & ~static_cast<size_t>(7);

	myWindowKey = WindowKey{};
	myWarpKey = WarpKey{};

	if (!myEngine) myEngine = std::make_unique<FFTDSP::FFTWEngine>();
	myEngine->prepare(myFFTSize, myPlanner, myLog, myBackend);

	for (auto& ch : myChannels) {
		ch.padded_frame.assign(myFFTSize, 0.0f);
		ch.rfft_magnitude.assign(FFTDSP::PerceptualWarping::linearBinCount(myFFTSize), 0.0f);
		ch.scratch_complex.resize(FFTDSP::PerceptualWarping::linearBinCount(myFFTSize));
		ch.prev_spectrum.clear();
		ch.agc_peak = 0.0f;
	}
	++myStatusVersion;
}

void
AnalysisPipeline::updateWindow(const Parameters::Values& p)
{
	WindowKey key{ static_cast<int>(p.window), p.kaiserBeta, myCapacity, static_cast<int>(p.magNorm) };
	if (key == myWindowKey && myWindowBuffer.size() == myCapacity) return;
	FFTDSP::WindowGenerator::generateWindow(static_cast<int>(p.window), p.kaiserBeta, myCapacity, myWindowBuffer,
	                                        p.magNorm == Parameters::MagNorm::FullScale ? FFTDSP::WindowNorm::FullScale
	                                                                                    : FFTDSP::WindowNorm::CoherentGain);
	myWindowKey = key;
}

/*
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
	// Display Max is clamped to Nyquist: above it there are no bins to show, and a larger axis would
	// just leave the top of the output empty and the bin spacing wrong.
	const double fmax = std::min(p.displayMax, nyquist);
	const int    scale = static_cast<int>(p.scale);
	const double blend = p.warp;
	const size_t n_out = static_cast<size_t>(outputBinCountFrom(p));
	const int    interp = static_cast<int>(p.warpInterp);

	WarpKey key{ scale, fmax, static_cast<int>(n_out), blend, p.logFloor, n_linear_bins, nyquist, interp };
	if (key == myWarpKey) return;
	myWarping.setInterpolation(interp);
	myWarping.buildWarpTables(scale, fmax, n_out, nyquist, blend, p.logFloor, n_linear_bins);
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

void
AnalysisPipeline::updateWeighting(const Parameters::Values& p)
{
	WeightKey key{ static_cast<int>(p.weighting), myWarpVersion };
	if (key == myWeightKey && myWeightingCurve.size() == myWarping.outputBins()) return;
	FFTDSP::EqualLoudness::computeCurve(static_cast<int>(p.weighting), myWarping.targetHz(), myWeightingCurve);
	myWeightKey = key;
}

void
AnalysisPipeline::runChannel(DspState& st, const FFTDSP::AlignedVector& window_in, bool silent, const Parameters::Values& p,
                             float attackCoef, float releaseCoef, float agcDecay, FFTDSP::AlignedVector& out) noexcept
{
	const size_t bins = myWarping.outputBins();
	assert(out.size() == bins);   // process() pre-sizes spectra; a size mismatch here is an invariant violation, not an allocation

	// Digital silence: nothing to analyse. (dB modes need the floor value and ballistics need to decay,
	// so the short-circuit only applies to the plain linear-magnitude path.)
	if (silent && p.loudness == Parameters::Loudness::Off && !p.ballEnable) {
		std::memset(out.data(), 0, bins * sizeof(float));
		st.prev_spectrum.clear();
		return;
	}

	// 1. window into the aligned centre of the persistent zero-padded frame
	size_t win_len = std::min({ myCapacity, myWindowBuffer.size(), window_in.size() });
	if (st.padded_frame.size() >= myPadStart + win_len) {
		FFTDSP::multiplyInto(window_in.data(), myWindowBuffer.data(), st.padded_frame.data() + myPadStart, win_len);
	}

	// 2. FFT + magnitude (only the bins the warp will read)
	myEngine->executeRFFT(st.padded_frame, st.rfft_magnitude, st.scratch_complex, myMagnitudeBins);

	// 2b. full-scale normalization: DC / Nyquist have no mirror bin
	if (p.magNorm == Parameters::MagNorm::FullScale && st.rfft_magnitude.size() >= 2) {
		st.rfft_magnitude.front() *= 0.5f;
		st.rfft_magnitude.back() *= 0.5f;
	}

	// 3. psychoacoustic warp (linear or cubic; identity memcpy when 1:1)
	myWarping.applyWarp(st.rfft_magnitude, out);

	// 4. equal-loudness weighting
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
		switch (p.dbRef) {
			case Parameters::DbRef::Dbfs:
				ref = (p.magNorm == Parameters::MagNorm::FullScale) ? 1.0f : static_cast<float>(myCapacity) * 0.5f;
				break;
			case Parameters::DbRef::Agc:
				st.agc_peak = std::max(peak, st.agc_peak * agcDecay);
				ref = st.agc_peak;
				break;
			case Parameters::DbRef::FramePeak:
			default:
				ref = peak;
				break;
		}
		if (!(ref > 0.0f)) ref = 1.0f;
		FFTDSP::DecibelConverter::convertToDB(loudness, p.dbRange, 1.0f / ref, out);
	} else if (st.prev_loudness_mode != 0) {
		st.prev_spectrum.clear();
	}
	st.prev_loudness_mode = loudness;

	// 6. ballistics (bypassed at 0/0)
	if (attackCoef > 0.0f || releaseCoef > 0.0f) {
		FFTDSP::BallisticsFilter ball;
		// apply() reads `current` and writes the smoothed frame into prev_out; it never writes through
		// `current`. So the smoothed result has to be copied back into the buffer the caller publishes,
		// and prev_spectrum keeps only the history the next frame smooths against.
		ball.apply(attackCoef, releaseCoef, out, st.prev_spectrum);
		std::memcpy(out.data(), st.prev_spectrum.data(), bins * sizeof(float));
	}
}

void
AnalysisPipeline::process(const AnalysisJob& job, AnalysisResult& res)
{
	const auto t0 = clk::now();
	const Parameters::Values& p = job.p;

	const bool rebuild_needed = !myEngine
	                         || std::abs(job.sampleRate - mySampleRate) > 1e-3
	                         || static_cast<size_t>(job.winSamples) != myCapacity
	                         || p.padSize != myPadChoice
	                         || static_cast<FFTDSP::PlannerPolicy>(p.planner) != myPlanner
	                         // A backend switch must rebuild: the plan object belongs to the library
	                         // that made it, so the old plan has to be destroyed by its own
	                         // fftwf_destroy_plan before the new one replaces it. prepare() does that.
	                         || &FFTDSP::backendById(static_cast<int>(p.backend)) != myBackend;
	if (rebuild_needed) rebuild(job);
	// The Async toggle, handed to the engine every cook before it is polled. Async off must mean one
	// thread for the whole node - the cook thread - and the FFTW planner's deferred MEASURE/PATIENT
	// upgrade is the one piece of work that would otherwise still run off it. The engine decides what
	// that means for a plan it already holds (see FFTWEngine::setBackgroundAllowed); this call is
	// deliberately unconditional and cheap, so a flip is never missed by the rebuild early-out.
	if (myEngine) myEngine->setBackgroundAllowed(p.async);
	if (myEngine->pollBackgroundPlan()) ++myStatusVersion;
	const bool upgrading = myEngine->upgradeInProgress();
	if (upgrading != myLastUpgrading) { myLastUpgrading = upgrading; ++myStatusVersion; }

	if (myChannels.size() != static_cast<size_t>(job.numChannels)) {
		size_t old = myChannels.size();
		myChannels.resize(static_cast<size_t>(job.numChannels));
		for (size_t i = old; i < myChannels.size(); ++i) {
			myChannels[i].padded_frame.assign(myFFTSize, 0.0f);
			myChannels[i].rfft_magnitude.assign(FFTDSP::PerceptualWarping::linearBinCount(myFFTSize), 0.0f);
			myChannels[i].scratch_complex.resize(FFTDSP::PerceptualWarping::linearBinCount(myFFTSize));
		}
	}
	if (job.reset) {
		for (auto& ch : myChannels) { ch.prev_spectrum.clear(); ch.agc_peak = 0.0f; }
	}

	updateWindow(p);
	updateWarp(p);
	updateWeighting(p);

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
		std::vector<int> idx(static_cast<size_t>(job.numChannels));
		std::iota(idx.begin(), idx.end(), 0);
		std::for_each(std::execution::par, idx.begin(), idx.end(), [&](int ch) {
			const bool silent = ch < static_cast<int>(job.silent.size()) && job.silent[ch] != 0;
			runChannel(myChannels[static_cast<size_t>(ch)], job.windows[static_cast<size_t>(ch)], silent, p,
			           attackCoef, releaseCoef, agcDecay, out[static_cast<size_t>(ch)]);
		});
	} else {
		for (int ch = 0; ch < job.numChannels; ++ch) {
			const bool silent = ch < static_cast<int>(job.silent.size()) && job.silent[ch] != 0;
			runChannel(myChannels[static_cast<size_t>(ch)], job.windows[static_cast<size_t>(ch)], silent, p,
			           attackCoef, releaseCoef, agcDecay, out[static_cast<size_t>(ch)]);
		}
	}

	// Peak telemetry (channel 0) belongs here, not on the cook thread: the owner has the Hz table.
	res.peakMag = 0.0f;
	res.peakHz = 0.0f;
	if (!out.empty() && !out[0].empty()) {
		size_t max_idx = 0;
		res.peakMag = FFTDSP::findPeakWithIndex(out[0].data(), out[0].size(), max_idx);
		// targetHz() is the literal output grid, whatever built it, so the peak's Hz needs no special
		// case — not even for the identity grid, where the table is exactly index * sr/fft_size.
		const std::vector<double>& hz = myWarping.targetHz();
		if (max_idx < hz.size()) res.peakHz = static_cast<float>(hz[max_idx]);
	}
	myLastUs = usSince(t0);
}
