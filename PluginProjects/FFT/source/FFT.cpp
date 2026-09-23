/* Shared Use License: This file is owned by Derivative Inc. (Derivative)
* and can only be used, and/or modified for use, in conjunction with
* Derivative's TouchDesigner software, and only if you are a licensee who has
* accepted Derivative's TouchDesigner license or assignment agreement
* (which also govern the use of this file). You may share or redistribute
* a modified version of this file provided the following conditions are met:
*
* 1. The shared file or redistribution must retain the information set out
* above and this list of conditions.
* 2. Derivative's name (Derivative Inc.) or its trademarks may not be used
* to endorse or promote products derived from this file without specific
* prior written permission from Derivative.
*/

/**
 * ===========================================================================
 *                   REAL-TIME OPERATOR IMPLEMENTATION
 * ===========================================================================
 * Source File: FFT.cpp   (see FFT.h for the threading architecture)
 *
 * Per cook (cook thread):
 *   getOutputInfo : poll the parameters (the only getPar* calls anywhere in the cook path),
 *                   report bins x channels.
 *   execute       :
 *     1. FTZ/DAZ guard, input sample rate, frame delta, window length
 *     2. ingest: mono-mix / first / per-channel block -> optional EQ (new samples only) -> FIFO,
 *        digital-silence tracking
 *     3. write the windows into a free job slot and publish it (every cook)
 *          - Async on: nothing else; the worker polls the slot every 2 ms (a kernel wake-up is
 *            only issued when the worker went dormant after 500 ms without jobs), or
 *          - Async off: run the pipeline inline
 *     4. acquire the latest finished result (one atomic exchange) and memcpy it to the output
 *        (hold the previous spectrum when nothing new has been published)
 *     5. telemetry; deferred Textport log flush only when something was logged
 * ===========================================================================
 *
 * HOW TO READ THIS FILE
 * ---------------------
 * It is the TouchDesigner half of the plugin, and it is longer than the DSP half for one reason:
 * TouchDesigner calls a lot of small callbacks and each one has rules. The order to read in:
 *
 *   the anonymous namespace        - version numbers, the popup's three size constants, and the
 *                                    thread-priority helper. Every constant here is explained in
 *                                    place; the version numbers must be kept in step with
 *                                    CHANGELOG.md by hand.
 *   FillCHOPPluginInfo()           - the DLL's one-time registration: operator type, label, icon,
 *                                    author, and the CPU check that every instance then reads.
 *   FFT::getGeneralInfo()          - cook scheduling. Read the comment on cookEveryFrame before
 *                                    changing it; it is load-bearing for the whole telemetry story.
 *   FFT::getOutputInfo()           - where the parameters are read. The only getPar* calls in a cook.
 *   FFT::execute()                 - THE COOK. Five numbered steps; this is the function to read if
 *                                    you want to know what happens 60 times a second.
 *   FFT::runJob()                  - what runs on the worker thread (or inline when Async is off)
 *   ingest() and friends           - how the input block becomes the window the FFT sees
 *   the info callbacks at the end  - getInfoCHOPChan/getInfoDATEntries/getInfoPopupString. These
 *                                    are the ONLY way this node surfaces diagnostics, and they run
 *                                    inside a cook, never on demand. See README, "The middle-click
 *                                    info popup".
 *
 * THE ONE THING TO UNDERSTAND BEFORE EDITING: everything the user sees about the node's state
 * (peak frequency, timings, plan events, the live backend, warnings, errors) travels through the
 * information callbacks, and those are only invoked as part of a cook. There is no "refresh" path.
 * ===========================================================================
 */

#include "FFT.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <execution>
#include <numeric>
#include <string>

namespace {

// Set once per process in FillCHOPPluginInfo() from FFTDSP::cpuSupportsAVX2(), then copied into every
// FFT instance. It exists as a namespace-scope variable rather than a per-instance field because the
// answer cannot change during a run: it is a property of the machine, and probing CPUID for every
// node in a project would be pointless work. Instances read the copy (myCpuOk) so a hot path never
// touches a global. See DSPModules.h for what the probe actually checks and why AVX2 alone is not
// enough (FMA is required too).
bool g_cpuHasAVX2 = true;

const char* kOpType    = "Fftcustom";   // must not collide with the built-in FFT CHOP
const char* kOpLabel   = "FFT Custom";
const char* kOpIcon    = "FFT";
// The three version numbers reported in the popup's `Plugin:` line and by the Info DAT. They are
// maintained by hand - nothing derives them from git - so a release that forgets to bump them makes
// the popup lie about which build is loaded, which is exactly what happened for v2.9.0. The current
// release these describe is v2.9.1 (2, 9, 1 below): keep them in step with CHANGELOG.md, and
// remember the popup's `Plugin:` line is the only place in TouchDesigner that names the build.
#ifndef FFT_VERSION_MAJOR
#define FFT_VERSION_MAJOR 2
#define FFT_VERSION_MINOR 10
#define FFT_VERSION_PATCH 0
#endif
// Generated from PluginProjects/FFT/plugin.json by CMakeLists.txt (FFT_VERSION_* definitions), so the
// popup's `Plugin:` line, the Custom OP version and plugin.json can no longer drift apart (they did for
// v2.9.0 and v2.9.1). The literals above are only the fallback for a build that bypasses that CMake.
const int   kMajorVersion = FFT_VERSION_MAJOR;
const int   kMinorVersion = FFT_VERSION_MINOR;
const int   kPatchVersion = FFT_VERSION_PATCH;

// Upper bound on the middle-click popup string, how many plan-log lines may be appended into it, and how much
// of each of those lines may be shown.
//
// The popup is the one string this plugin hands to TouchDesigner whose rendering has been observed to depend
// on its length: ~1660 characters rendered, ~1760 came up empty (measured, both directions - see README,
// "The middle-click info popup"). No cap is documented anywhere in the SDK - OP_String::setString takes a
// NUL-terminated const char* and says only that it is UTF-8 - so there is no limit to design against, only
// lengths that are known to have worked and known to have failed.
//
// The previous bound was 1600, which was not a bound on this string at all: it guarded the tail loop only, so
// the fixed body could sit at ~780 characters and the first tail line could add 240 more before anything was
// checked. It was also within ~60 characters of the shortest length known to fail. Both are fixed here by
// making the bound apply to the whole string (see the `add` lambda in getInfoPopupString) and by moving it
// well clear of the observed break, so the popup's length is now a constant the reader can predict rather
// than a number that drifts with whatever the engine last logged.
constexpr size_t kMaxPopupChars     = 1200;
// How many plan-log lines the popup renders. Named because two more places depend on the number: the
// Info DAT's row count is derived from what is left after the fixed rows, and bench.cpp's --info
// measurement mirrors this value (it is file-local here, so it is repeated there with a note).
constexpr size_t kTailPlanLogLines  = 3;
// Characters of each plan-log line the popup shows. A plan line is ~240 characters because the engine's
// backend description embeds the absolute path of the FFT library (see describeBackend), and three of those
// were 70 % of this string: the popup's length moved by hundreds of characters depending on which plan events
// happened to be last, which is exactly the variation that makes a length-dependent failure look random. The
// full line is in the Info DAT's plan_log_* rows; the popup shows which events happened and the head of each.
constexpr size_t kMaxTailLineChars  = 72;

using clk = std::chrono::steady_clock;
// Microseconds since a steady-clock timestamp, for the cook and DSP timings. steady_clock rather
// than system_clock because this measures elapsed time and must not jump if the clock is adjusted.
inline double usSince(clk::time_point t0) { return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); }

// The TD-free sample-rate / bin / axis model (windowSamplesFrom, fftSizeFrom,
// outputBinCountFrom, axisRate, sampleRateToTouchDesigner, hzPerBin, throughput) lives in
// RateModel.h so it is shared by the CHOP and the pipeline and is unit-testable headlessly.
// (RateModel.h is included at global scope below / via FFT.h: its functions are all inline.)

// (The worker thread's naming, priority, MMCSS registration and power-throttling opt-out moved to
// AsyncAnalysis.cpp together with the worker itself.)

} // namespace

// =============================================================================================
// C-ABI entry points (C++ API 10)
// =============================================================================================
extern "C"
{

DLLEXPORT
void
FillCHOPPluginInfo(CHOP_PluginInfo* info)
{
	if (!info->setAPIVersion(CHOPCPlusPlusAPIVersion))
		return;

	g_cpuHasAVX2 = FFTDSP::cpuSupportsAVX2();

	info->customOPInfo.opType->setString(kOpType);
	info->customOPInfo.opLabel->setString(kOpLabel);
	info->customOPInfo.opIcon->setString(kOpIcon);
	info->customOPInfo.authorName->setString("NairoDorian");
	info->customOPInfo.authorEmail->setString("Nairod785@gmail.com");
	info->customOPInfo.minInputs = 1;
	info->customOPInfo.maxInputs = 1;
	// cookOnStart kick-starts every-frame cooking for a node nothing else in the file uses or views,
	// which matters because all of this plugin's telemetry arrives through the info callbacks and
	// those only run inside a cook (see getGeneralInfo below). It applies to one of the two ways this
	// DLL gets loaded, and it is worth being exact about which:
	//
	//   * As a registered Custom Operator (Documents/Derivative/Plugins/FFT) - honoured, and needed.
	//   * Loaded into the built-in CPlusPlus CHOP - ignored. The SDK says so explicitly ("this fix
	//     only works for Custom Operators, not cases where the .dll is loaded into CPlusPlus CHOP"),
	//     and that is how PluginBuilder hosts it (plugin_loader is a cplusplusCHOP).
	//
	// So this line is not what makes the info popup work in the PluginBuilder setup; cookEveryFrame in
	// getGeneralInfo is. It is set anyway because the operator must behave correctly however it is
	// deployed, and because a node that never cooks on project load has no telemetry to show until
	// something happens to pull it.
	info->customOPInfo.cookOnStart = true;
	info->customOPInfo.majorVersion = kMajorVersion;
	info->customOPInfo.minorVersion = kMinorVersion;
	info->customOPInfo.opHelpURL->setString("https://github.com/NairoDorian/TD_Custom_FFT");
}

DLLEXPORT
CHOP_CPlusPlusBase*
CreateCHOPInstance(const OP_NodeInfo* info)
{
	return new FFT(info);
}

DLLEXPORT
void
DestroyCHOPInstance(CHOP_CPlusPlusBase* instance)
{
	delete static_cast<FFT*>(instance);
}

} // extern "C"

// =============================================================================================
// FFT operator
// =============================================================================================
// Instance construction. Order matters: the CPU check is copied first because a machine without
// AVX2/FMA must end up with an error text set before anything tries to run a transform, and the
// log must be put into deferred mode before the pipeline is built (the pipeline can log from its
// worker thread, and a worker thread must never call into Python - see PlanLog in DSPModules.h).
FFT::FFT(const OP_NodeInfo* info)
	: myNodeInfo(info)
{
	myCpuOk = g_cpuHasAVX2;
	if (!myCpuOk) {
		myErrorText = "This CPU has no AVX2/FMA support; the FFT plugin is compiled for AVX2 and will output silence.";
		myLog.log("[FFT Plugin] ERROR: " + myErrorText);
	}
	myLog.setDeferred(true);   // the worker must never call into Python; the cook thread flushes
	myPipeline = std::make_unique<AnalysisPipeline>(&myLog);
	myAsync = std::make_unique<AsyncAnalysis>(*myPipeline, myLog);
}

// Destroying the operator stops the worker and joins it before the pipeline is destroyed (the
// member's own destructor runs after this body). That order matters: the worker thread calls
// into the pipeline, so the pipeline must outlive it.
FFT::~FFT()
{
	myAsync.reset();   // joins the worker before the pipeline it calls into is destroyed
}

void
FFT::getGeneralInfo(CHOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1)
{
	// cookEveryFrame, not cookEveryFrameIfAsked. This is load-bearing, not a preference.
	//
	// TouchDesigner calls every information callback *inside a cook*, in the order documented at the
	// top of CHOP_CPlusPlusBase.h: execute() -> getNumInfoCHOPChans()/getInfoCHOPChan() ->
	// getInfoDATSize()/getInfoDATEntries() -> getInfoPopupString() -> getWarningString() ->
	// getErrorString(). Nothing else calls them, and none of them is invoked on demand by the
	// middle-click itself - the popup renders whatever the last cook left behind.
	//
	// v2.3.0 set cookEveryFrame = false / cookEveryFrameIfAsked = true, which the SDK defines as
	// "if nobody is using the output from the CHOP, it won't cook". That is the right economy for a
	// pure number-cruncher, and the wrong one for this node, which carries all of its telemetry
	// (peak frequency, cook and DSP time, plan events, live backend, warnings, errors) through those
	// callbacks. An idle node therefore has no Info CHOP, no Info DAT, and an *empty* middle-click
	// info popup - the callbacks are correct, they are simply never reached.
	//
	// Worse, a hard failure cannot report itself: getErrorString() is in that same chain, so a plan
	// that will not build or an input with no usable sample rate leaves the node silently blank
	// instead of flagged. Always cooking is what keeps the node able to explain itself.
	//
	// The cost is the async pipeline's cook-thread work, measured at 11 us mean / 17 us p99 per cook
	// at 16384 bins (v2.4.0, `fft_bench --cook`) - about 0.07 % of a 60 fps frame. See cookOnStart in
	// fillCustomOPInfo(): the SDK requires it alongside cookEveryFrame to kick-start cooking, since
	// a node nobody pulls is never asked to cook in the first place.
	ginfo->cookEveryFrame = true;
	ginfo->cookEveryFrameIfAsked = false;
	ginfo->timeslice = false;
	// "cook in lockstep with input 0's rate" - the standard choice for a node that processes a
	// signal, and unchanged since the first version. It has no effect on scheduling.
	ginfo->inputMatchIndex = 0;
}

// ---------------------------------------------------------------------------------------------
// Parameters: one eval() per cook, from getOutputInfo() (TouchDesigner calls it right before
// execute()). execute() only re-polls if getOutputInfo() was skipped for this cook.
// ---------------------------------------------------------------------------------------------
// Reads every parameter once and stores the result in myParams. This is the ONLY place getPar*
// is called in the cook path, and it is deliberately done in one batch: TouchDesigner's parameter
// lookups are not free, and doing them per use would scatter them through execute().
//
// Called from getOutputInfo() (which TouchDesigner invokes immediately before execute()) and, as a
// fallback, from execute() itself if that call was skipped. myHaveParams/myParamsFreshForExecute
// record that it happened, so the second call is a no-op rather than a double read.
// ---------------------------------------------------------------------------------------------
void
FFT::pollParameters(const OP_Inputs* inputs)
{
	const auto t0 = clk::now();
	myParams = Parameters::eval(inputs, &myParamReads);
	myHaveParams = true;
	myParamUs = usSince(t0);   // reported as param_us; part of the cook-time breakdown on the Info DAT
}

// How many spectra this cook will produce. Mono Mix and "First Channel" always analyse one channel
// (a mono mix, or the first channel of the input); All Channels analyses every input channel, up to
// the hard cap Parameters::kMaxChannels.
//
// The cap is not cosmetic: each analysed channel costs a full FFT per frame and gets its own
// per-channel buffers, so an input with hundreds of channels would multiply the per-cook cost by
// hundreds. One channel unless explicitly asked, and a bounded number when asked, is the whole rule.
// A missing input (nullptr) or a channel-less one still reports 1, so the node always has a shape.
int
FFT::analysisChannelCount(const OP_CHOPInput* cinput, Parameters::ChanMode mode) const
{
	if (!cinput || cinput->numChannels <= 0) return 1;
	if (mode == Parameters::ChanMode::AllChannels) return std::clamp(cinput->numChannels, 1, Parameters::kMaxChannels);
	return 1;
}

// The band the frequency axis covers, expressed in the standard "bin 0 is DC, the last bin is
// Nyquist" form: 2 * (top of the axis). Display Max is clamped to Nyquist, so this is 2 * min(Display
// Max, nyquist) — the input rate when Display Max reaches the top of the band. It does NOT depend on
// how many Output Bins describe the band.
//
// This is what hzPerSample() and the `output_spectrum_axis` Info row are derived from. It is NOT
// what info->sampleRate reports — see outputSampleRate() below.
//
// The two readings of "sample rate" this file keeps apart, in one line each:
//   outputAxisRate()   - what the bins MEAN (the band the axis covers)   -> hz_per_sample, axis rows
//   outputSampleRate() - how FAST the data comes out (bins x fps)        -> info->sampleRate
double
FFT::outputAxisRate(const Parameters::Values& p, double sampleRate) const
{
	// Thin delegate to the TD-free, unit-tested axisRate() in RateModel.h; reads the axis rate the
	// last pipeline published (0 when nothing has run yet, which axisRate falls back to 2*fmax).
	return axisRate(p, sampleRate, myAsync ? myAsync->axisRate() : 0.0);
}

// Sample rate reported to TouchDesigner for the spectrum, as requested: one output vector of
// `bins` samples is produced every 1/me.time.rate seconds, so the node emits bins * rate samples
// per second. At the defaults (16384 bins, 60 fps) that is 983 040.
//
// This is the CONCATENATED-STREAM reading of the output: it is the rate you get if you string one
// frame's spectrum after another into a single signal, and it is what sizes a buffer, a ring, a GPU
// upload or a network send. It is a property of the refresh cadence, not of the spectrum itself —
// so it carries no bin-index-to-Hz information. The axis (and therefore every frequency you read
// off this CHOP) lives in outputAxisRate()/hzPerSample(), which the Info CHOP exposes as
// `hz_per_sample` and `output_spectrum_axis`. Use those to convert a bin index to Hz; the sample
// rate now tells you how fast the data is coming, not what the bins mean.
//
// Deliberately takes no input rate: this number is `bins` x the cook rate and nothing else. Passing
// the audio rate in here is what used to make it wrong.
double
FFT::outputSampleRate(const Parameters::Values& p) const
{
	// Delegate to the TD-free, unit-tested sampleRateToTouchDesigner() in RateModel.h.
	return sampleRateToTouchDesigner(p, mySampleRate, myCookRate.load(std::memory_order_relaxed));
}

// Hz per output bin — the index-to-Hz mapping, and now the only channel that carries it, since
// info->sampleRate reports throughput (see outputSampleRate above). The axis covers
// outputAxisRate()/2 Hz over (bins - 1) intervals, so this is exactly fmax/(bins-1): the true
// frequency resolution of the grid that was built. A uniform axis (Scale = Linear, whose perceptual
// ramp IS the linear one, or Warp Blend = 0, which ignores the scale entirely) has this spacing
// everywhere; a perceptual grid does not, and there the mean is reported, which is the only single
// number that can describe it. TouchDesigner's Audio Spectrum CHOP exposes its equivalent as
// `hz_per_sample`, for the same reason: the sample rate cannot carry the index-to-Hz mapping once a
// grid is resampled.
double
FFT::hzPerSample(const Parameters::Values& p, double sampleRate) const
{
	// Delegate to the TD-free, unit-tested hzPerBin() in RateModel.h.
	return hzPerBin(p, sampleRate, myAsync ? myAsync->axisRate() : 0.0);
}

// Data throughput of the node, in samples per second — deliberately NOT the sample rate.
//
// This is the same quantity as outputSampleRate(), but measured instead of declared: `bins` samples
// actually left the node over the cook delta we actually observed, rather than over the nominal
// 1/me.time.rate. It is the figure to sanity-check a buffer, a ring, a GPU upload or a network send
// with, and the rate of a signal you get by concatenating frames.
//
// It exists as a separate number because info->sampleRate means two different things by convention,
// and this node has to pick one. A CHOP sample rate normally says how far apart the samples inside
// one output vector are; for a spectrum that is a FREQUENCY spacing, not a time one. What
// TouchDesigner does with `sampleRate` downstream (an Audio Spectrum CHOP's bin-to-Hz maths, for
// one) assumes the time reading, so the honest choice for a spectrum is the throughput reading —
// see outputSampleRate() for why the node reports bins x me.time.rate and not the input rate.
double
FFT::outputBandwidth(const Parameters::Values& p) const
{
	// Delegate to the TD-free, unit-tested throughput() in RateModel.h.
	return throughput(p, mySampleRate, myCookDtMs.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------------------------
// getOutputInfo - TouchDesigner asks what shape the output will be, immediately before execute().
// This is where the parameters are read (the only getPar* calls in a cook), and where the CHOP's
// declared size, channel count and sample rate are set. The frame delta is also captured here.
// ---------------------------------------------------------------------------------------------
bool
FFT::getOutputInfo(CHOP_OutputInfo* info, const OP_Inputs* inputs, void* reserved1)
{
	pollParameters(inputs);
	myParamsFreshForExecute = true;
	// me.time.rate for this cook — the timeline rate where this node lives, which can differ from
	// the root rate inside a component with Component Time. The reported sample rate is
	// bins * this, so read it here: getOutputInfo needs it now, and it also publishes it for the
	// Info CHOP/DAT callbacks, which never receive an OP_Inputs of their own.
	if (const OP_TimeInfo* ti = inputs->getTimeInfo()) {
		if (ti->rate > 0.0) myCookRate.store(ti->rate, std::memory_order_relaxed);
	}
	const OP_CHOPInput* cinput = (inputs->getNumInputs() > 0) ? inputs->getInputCHOP(0) : nullptr;
	const Parameters::Values& p = myParams;
	// Output Bins Mode = Auto sizes the output from the window's resolution, which depends on the input
	// rate: use the same clamped rate executeImpl will hand the pipeline, so the declared width and the
	// grid the pipeline builds agree.
	const double sr = std::clamp((cinput && cinput->sampleRate > 0) ? cinput->sampleRate : 44100.0,
	                             Parameters::kMinSampleRate, Parameters::kMaxSampleRate);
	const int bins = outputBinCountFrom(p, sr);
	myOutputBins = bins;
	info->startIndex = 0;
	info->numSamples = bins;
	info->numChannels = analysisChannelCount(cinput, p.chanMode);
	// Note this is the throughput reading, not the axis rate - see outputSampleRate() above and the
	// `output_spectrum_axis`/`hz_per_sample` Info rows for the frequency meaning of the bins.
	info->sampleRate = static_cast<float>(sampleRateToTouchDesigner(p, sr, myCookRate.load(std::memory_order_relaxed)));
	return true;
}

// The name of each output channel. Convention: the input channel's own name with "_fft" appended,
// so a spectrum is recognisable next to its source in a network; "mix_fft" for the mono mix and
// "rfft" for the fallback with no usable input. Cosmetic only - nothing reads these names back.
void
FFT::getChannelName(int32_t index, OP_String* name, const OP_Inputs* inputs, void* reserved1)
{
	const OP_CHOPInput* cinput = (inputs->getNumInputs() > 0) ? inputs->getInputCHOP(0) : nullptr;
	if (cinput && cinput->numChannels > 0) {
		if (myParams.chanMode == Parameters::ChanMode::MonoMix && cinput->numChannels > 1) {
			name->setString("mix_fft");
			return;
		}
		if (index < cinput->numChannels) {
			char buf[128];
			snprintf(buf, sizeof(buf), "%s_fft", cinput->getChannelName(index));
			name->setString(buf);
			return;
		}
	}
	name->setString("rfft");
}

// Writes zeros over every channel TouchDesigner handed us, and does nothing else.
//
// WHERE IT IS CALLED FROM, and there are exactly three sites: the `!myCpuOk` early-out in
// executeImpl(), and the two catch blocks in execute() (std::exception and ...).
//
// WHY IT IS SO DEFENSIVE: every one of those is a failure path, i.e. exactly when the state of
// `output` is least trustworthy. So it checks the pointer, checks the channel array, clamps the
// channel count against both the reported count and the hard cap, and checks each channel pointer
// before writing to it. A crash here would replace a silent node with a crashed TouchDesigner,
// which is a strictly worse outcome than silence.
//
// noexcept for the same reason: an exception thrown while already handling a failure has nowhere
// to go, and would terminate the process.
void
FFT::zeroOutputSafe(CHOP_Output* output) noexcept
{
	if (!output || !output->channels) return;
	int chans = std::max(0, std::min(output->numChannels, Parameters::kMaxChannels));
	int samples = std::max(0, output->numSamples);
	for (int c = 0; c < chans; ++c) {
		if (output->channels[c]) std::memset(output->channels[c], 0, samples * sizeof(float));
	}
}

// ---------------------------------------------------------------------------------------------
// Ingest (cook thread): mono mix / first / all -> optional EQ on the new samples -> FIFO
// ---------------------------------------------------------------------------------------------
// WHAT THIS DOES: turns this cook's input block into one sample stream per analysis channel, then
// pushes it into that channel's FIFO (the ring buffer that always holds the newest `capacity`
// samples - the window the next FFT will be taken over).
//
// THE FOUR STEPS, in the order they must happen:
//   1. pick the source block - the mono mix of every input channel, or one specific channel
//   2. note whether the block is digital silence (a run counter, used to skip work later)
//   3. run the optional EQ over the new samples, in time order
//   4. push the block into the FIFO
//
// WHY THE EQ COMES BEFORE THE FIFO AND NOT AFTER: EQ is a stateful filter (it carries its own
// history from the previous block). To stay continuous it must see samples in the order they were
// captured, once each. Running it after the FIFO would filter the same samples repeatedly, once per
// cook, as the window slides - which would both multiply the cost and change the result.
//
// WHY ONLY THE NEW SAMPLES: `keep = min(numSamples, capacity)`. A cook normally delivers far fewer
// samples than the window is long (60 frames/s against a 3175-sample window), so only the tail of
// the block is new information; the rest is already in the FIFO. Copying more would be wasted work.
//
// CALLED BY: execute(), step 2, once per cook. Cook thread only - it writes the FIFOs, and
// fillJob() reads them on the same thread.
void
FFT::ingest(const OP_CHOPInput* cinput, Parameters::ChanMode mode, int numChannels, const Parameters::Values& p, size_t capacity)
{
	if (static_cast<int>(myIngest.size()) != numChannels) myIngest.resize(static_cast<size_t>(numChannels));
	// Housekeeping over every state, not just the ones this cook will use: the loop is a
	// capacity compare in the steady state, and doing it for all of them means a state left over
	// from a previous, larger channel count is already consistent if the count grows back.
	for (auto& st : myIngest) {
		// A capacity change means a different window length, i.e. a different FFT. The FIFO is
		// reset rather than carried over because its contents describe a window that no longer
		// exists; silent_run restarts with it for the same reason.
		if (st.fifo.capacity() != capacity) { st.fifo.resize(capacity); st.silent_run = 0; }
		// The EQ's coefficients are sample-rate dependent, so it is told the current rate every
		// cook (it is a cheap no-op when the rate has not changed).
		st.eq.setSampleRate(mySampleRate);
	}
	// Nothing to ingest: no input connected, or a CHOP with no channels, or a cook with no samples
	// in it. Returning here leaves the FIFOs untouched, which is deliberate - the spectrum keeps
	// showing the last real audio rather than falling to zero on a momentary gap.
	if (!cinput || cinput->numChannels <= 0 || cinput->numSamples <= 0) return;

	const size_t n = static_cast<size_t>(cinput->numSamples);
	// How many of this block's samples are NEW (IngestMode::Auto, v2.10). Before 2.10 every cook's whole
	// block was appended, which is right for a timesliced audio input but appends the same samples again
	// for an input that re-delivers an overlapping or identical buffer (a sliding non-timesliced window,
	// a static buffer, an input that did not cook this frame) - the "window" became a stutter of repeats.
	//   * the input did not cook since the last ingest (same totalCooks): nothing is new;
	//   * its range advanced by d samples (startIndex + numSamples): the newest min(d, n) are new;
	//   * a range that went backwards or did not move although the input re-cooked (a reset, a looping
	//     file, a generator that keeps startIndex): the whole block is new (the legacy behaviour).
	const size_t fresh = (p.ingestMode == Parameters::IngestMode::Auto)
		? myIngestCursor.fresh(cinput->startIndex, n, cinput->totalCooks)   // RateModel.h, unit-tested
		: n;
	if (fresh == 0) return;
	const size_t keep = std::min(fresh, capacity);             // only samples that will still be inside the window
	const int inChans = cinput->numChannels;

	for (int ch = 0; ch < numChannels; ++ch) {
		IngestState& st = myIngest[static_cast<size_t>(ch)];
		if (st.block.size() < keep) st.block.resize(keep);     // grows once, then reused every cook
		float* blk = st.block.data();

		// --- source block ---
		// The `+ (n - keep)` everywhere below is the point of `keep`: it skips the older samples at
		// the front of the block and takes only its newest `keep` ones, which are the only ones not
		// already in the FIFO.
		if (mode == Parameters::ChanMode::MonoMix && inChans > 1) {
			// Sum every input channel into the scratch block, then divide by the channel count so
			// the mix does not get louder with more channels (an average, not a sum).
			const float* c0 = cinput->getChannelData(0);
			if (!c0) continue;
			std::memcpy(blk, c0 + (n - keep), keep * sizeof(float));
			for (int c = 1; c < inChans; ++c) {
				const float* cd = cinput->getChannelData(c);
				if (cd) FFTDSP::addInto(blk, cd + (n - keep), blk, keep);
			}
			FFTDSP::scaleInPlace(blk, keep, 1.0f / static_cast<float>(inChans));
		} else {
			// One channel only: the first input channel for "First Channel" and Mono Mix with a
			// single input, or channel `ch` for All Channels - clamped with std::min so a request
			// for more analysis channels than the input has repeats the last real channel instead
			// of reading past the end.
			const int src_ch = (mode == Parameters::ChanMode::AllChannels) ? std::min(ch, inChans - 1) : 0;
			const float* cd = cinput->getChannelData(src_ch);
			if (!cd) continue;
			std::memcpy(blk, cd + (n - keep), keep * sizeof(float));
		}

		// --- silence tracking (digital zero) ---
		// A run counter, not a per-cook flag: what the pipeline needs to know is "has this channel
		// been silent for a whole window's worth of samples", because that is the point at which the
		// spectrum is genuinely flat rather than merely quiet. The counter is capped at 2*capacity so
		// it cannot grow without bound over a long silence and overflow its int.
		if (FFTDSP::blockIsSilent(blk, keep)) st.silent_run = std::min(st.silent_run + keep, capacity * 2);
		else st.silent_run = 0;

		// --- EQ on the new samples only (stateful, time order) ---
		// updateAndCheckActive() reports whether the filter is doing anything at all this block: with
		// EQ disabled, or enabled but flat, it returns false and the filter is skipped entirely. That
		// is the difference between "EQ costs nothing when it is off" and "EQ costs a full pass over
		// every sample in the window on every cook".
		if (p.eqEnable && st.eq.updateAndCheckActive(p.gainDb, p.cutoffHz, p.lowGainDb, p.lowCutoffHz, p.q, p.amount)) {
			st.eq.processBlockInPlace(blk, keep, p.amount);
		}
		st.fifo.add(blk, keep);
	}
}

// ---------------------------------------------------------------------------------------------
// Job / result handoff
// ---------------------------------------------------------------------------------------------
// WHAT A JOB IS: one self-contained description of "analyse this". It carries the parameters as
// they were at this cook, the frame delta, the reset request, and a full copy of every channel's
// newest window. Nothing in it points back at the node, which is what lets the analysis run on
// another thread without touching live state.
//
// WHY THE WINDOWS ARE COPIED (fifo.get) RATHER THAN SHARED: the FIFO keeps receiving samples while
// the analysis of the previous frame is still running, so a pointer into it would be reading memory
// that is being rewritten underneath. The copy is a few thousand floats - cheaper than any
// synchronisation that would make sharing safe.
//
// CALLED BY: execute(), step 3, once per cook, on the cook thread. The caller publishes the filled
// slot afterwards (myJobs.publish()).
void
FFT::fillJob(AnalysisJob& job, int numChannels, int winSamples, double dtMs)
{
	// seq counts jobs, not cooks, and it is how a result is matched back to the job that produced it
	// (see copyResultsToOutput, where the difference is what the hold_frames figure counts).
	job.seq = ++myJobSeq;
	job.numChannels = numChannels;
	job.sampleRate = mySampleRate;
	job.winSamples = winSamples;
	job.dtMs = dtMs;
	job.p = myParams;                 // the pipeline never reads live UI state; see pollParameters()
	job.reset = myResetPending;       // consumes the Reset pulse; cleared at the end of this function
	const size_t n = static_cast<size_t>(numChannels);
	if (job.windows.size() != n) job.windows.resize(n);     // no allocation after the first cooks
	if (job.silent.size() != n) job.silent.resize(n);
	for (int ch = 0; ch < numChannels; ++ch) {
		myIngest[ch].fifo.get(job.windows[ch]);             // newest `capacity` samples, oldest first
		// One whole window of digital silence: not "quiet", but exactly zero, and long enough that
		// the spectrum is genuinely flat. The pipeline uses this to skip work it can prove is wasted.
		job.silent[ch] = myIngest[ch].silent_run >= myCapacity ? 1 : 0;
	}
	myResetPending = false;
}

// (runJob, startWorker, stopWorker and workerLoop moved to AsyncAnalysis.cpp in v2.10, unchanged in
// behaviour: latest-wins triple buffers, try/publish/republish-telemetry, the Dekker dormancy
// handshake. They are TD-free there, so tests/dsp_tests.cpp and bench/bench.cpp drive the same code.)

// Publishes the newest finished spectrum into the CHOP's output buffers, and updates the three
// telemetry values that describe it (peak frequency, peak magnitude, how many frames behind the
// analysis is).
//
// THE ONE LINE THAT MATTERS: myResults.acquire() is a single atomic exchange. If the pipeline has
// published a result since the last cook it becomes the front slot; if it has not, this is a no-op
// and the previous result stays where it is. That is the whole of the "hold the previous spectrum"
// behaviour - the node is never blank while waiting for the next analysis, it simply repeats the
// last one, and hold_frames says how many cooks that has been true for.
//
// EVERY OTHER LINE IS SIZING DEFENCE. The result's spectra and the output's buffers are sized by
// two independent code paths (getOutputInfo for the node, the pipeline for the spectrum), and a
// mismatch is possible at the moment a parameter changes. So: the copy is min(result, output), the
// remainder of the output is zeroed rather than left with whatever was in it, and a channel with no
// spectrum at all is zeroed outright.
//
// CALLED BY: execute(), step 4, once per cook, on the cook thread.
void
FFT::copyResultsToOutput(CHOP_Output* output, int numChannels)
{
	myAsync->acquireResult();                              // one atomic exchange; no-op when nothing new
	const AnalysisResult& res = myAsync->result();
	const size_t out_samples = static_cast<size_t>(std::max(0, output->numSamples));
	for (int ch = 0; ch < numChannels && ch < output->numChannels; ++ch) {
		float* dst = output->channels[ch];
		if (!dst) continue;
		if (ch < static_cast<int>(res.spectra.size()) && !res.spectra[ch].empty()) {
			const FFTDSP::AlignedVector& src = res.spectra[ch];
			size_t n = std::min(out_samples, src.size());
			std::memcpy(dst, src.data(), n * sizeof(float));
			if (out_samples > n) std::memset(dst + n, 0, (out_samples - n) * sizeof(float));
		} else {
			std::memset(dst, 0, out_samples * sizeof(float));
		}
	}
	// The peak is measured by the pipeline on channel 0 only (see AnalysisResult in
	// AnalysisPipeline.h); it is read here rather than searched for so the Info callbacks never
	// walk a spectrum.
	myPeakMagnitude = res.peakMag;
	myPeakFrequencyHz = res.peakHz;
	myHaveFeatures = res.hasFeatures;
	if (res.hasFeatures) myFeatures = res.features;         // 8 floats: a copy, no allocation
	// How far behind the analysis is: the number of jobs published since the one that produced this
	// result. 0 = this cook is showing a spectrum from this cook's own job; 1 = the normal
	// one-frame latency of the async pipeline. The guard keeps a result from a previous sequence
	// (which cannot normally be seen, but would underflow an unsigned difference) from reporting a
	// nonsense number.
	myHoldFrames = (res.seq <= myJobSeq) ? static_cast<int>(myJobSeq - res.seq) : 0;
}

// ---------------------------------------------------------------------------------------------
// Cook
// ---------------------------------------------------------------------------------------------
// THE COOK, step by step. Everything a cook does happens here; execute() below is only the
// try/catch wrapper that turns an exception into an error string and a silent output.
//
// THE FIVE STEPS, in order (the numbers in the code match these):
//   0. measure the gap since the previous cook, for the "largest cook gap" diagnostic
//   1. read the parameters, the input sample rate and the frame delta, and work out the window
//      length this cook will use
//   2. ingest: build each channel's sample stream and push it into its FIFO
//   3. hand the analysis job to whoever will run it - the worker thread when Async is on, or this
//      same thread when it is off
//   4. copy the newest finished spectrum into the output buffers
//   5. finish: flush anything the plan log wanted printed, and record how long the cook took
//
// THE ONE THING TO UNDERSTAND BEFORE EDITING: a cook does NOT necessarily run the analysis, and it
// does NOT necessarily see the result of the job it just published. With Async on, the spectrum
// written in step 4 usually belongs to the *previous* cook's job - that one-frame latency is the
// deal that keeps the FFT off the cook thread, and hold_frames reports it. So "the output is one
// frame stale" is the design, not a bug.
//
// myExecStage is set before each step and cleared at the end. Its only purpose is the error string:
// if something throws, the stage number written by execute() below says where.
void
FFT::executeImpl(CHOP_Output* output, const OP_Inputs* inputs)
{
	myExecuteCount++;
	// Defensive, in this order: an output with no buffers, or no OP_Inputs at all, cannot be
	// served. Leaving the buffers alone is better than zeroing them here, because the previous
	// cook's spectrum is still valid content.
	if (!output || !output->channels || !inputs) return;
	// No AVX2/FMA means every kernel in this plugin would fault or miscompute, so the node outputs
	// silence and reports why through getErrorString (the text was set in the constructor).
	if (!myCpuOk) { zeroOutputSafe(output); return; }

	const auto t_start = clk::now();
	// Flush-to-zero / denormals-are-zero for this thread. Denormal floats (values below ~1e-38) are
	// produced naturally by the tail of a decaying signal and by FFT round-off, and on x86 they are
	// handled in microcode - a single denormal in a loop can cost more than the entire transform.
	// The guard is a scope object so the previous mode is restored on every exit path, including
	// the throws that execute() catches. The worker thread sets it for itself as well (workerLoop).
	FFTDSP::DenormalGuard ftz;

	// --- 0. how long was the gap since the previous cook? --------------------------------------
	// This is the only place in the plugin that can observe a cook that did NOT happen. Every other
	// signal - the popup, the Info CHOP, the Info DAT, the warning and error strings - is produced by
	// callbacks TouchDesigner runs *inside a cook*, so if cooking stops they all go quiet together and
	// the node simply looks blank, with nothing anywhere saying why. Recording the largest gap means
	// that once cooking resumes (which is what a reload does), the popup can say it happened and for
	// how long, instead of the user being left with an empty box and no history. Two clock reads per
	// cook, on the cook thread, and no allocation.
	if (myLastCookStart.time_since_epoch().count() != 0) {
		const double gap_ms = std::chrono::duration<double, std::milli>(t_start - myLastCookStart).count();
		if (gap_ms > myMaxCookGapMs.load(std::memory_order_relaxed)) {
			myMaxCookGapMs.store(gap_ms, std::memory_order_relaxed);
		}
	}
	myLastCookStart = t_start;

	// --- 1. parameters (normally already polled by getOutputInfo for this cook) ---
	myExecStage = 1;
	if (!myParamsFreshForExecute) pollParameters(inputs);
	myParamsFreshForExecute = false;
	const Parameters::Values& p = myParams;

	const OP_CHOPInput* cinput = (inputs->getNumInputs() > 0) ? inputs->getInputCHOP(0) : nullptr;
	// The sample rate of the audio arriving on input 0. It is clamped rather than trusted: it comes
	// from another operator and a CHOP that has not been set up yet can report 0 or something
	// absurd, and every table in the pipeline is sized from it. 44100 is the fallback for "no input
	// connected" so that the node still produces a correctly shaped spectrum instead of nothing.
	double sr = 44100.0;
	myRawInputRate = cinput ? cinput->sampleRate : 44100.0;   // unclamped, for getErrorString
	if (cinput && cinput->sampleRate > 0) sr = cinput->sampleRate;
	mySampleRate = std::clamp(sr, Parameters::kMinSampleRate, Parameters::kMaxSampleRate);

	// The frame delta, i.e. how much time this cook represents. It is what makes the ballistics
	// (attack/release, AGC decay) frame-rate independent: those are specified in milliseconds and
	// converted with this figure, so the same settings sound the same at 30 fps and at 120 fps.
	// deltaMS is preferred and the rate is the fallback, because deltaMS is what actually elapsed
	// while `rate` is only the target - they differ whenever a frame runs long. deltaMS is sanity
	// checked against 5000 ms (a plausible pause or a first cook, where the frame delta can be
	// enormous and would otherwise collapse every ballistics coefficient to zero).
	double dt_ms = 1000.0 / 60.0;
	if (const OP_TimeInfo* ti = inputs->getTimeInfo()) {
		if (ti->deltaMS > 0.0 && ti->deltaMS < 5000.0) dt_ms = ti->deltaMS;
		else if (ti->rate > 0.0) dt_ms = 1000.0 / ti->rate;
		if (ti->rate > 0.0) myCookRate.store(ti->rate, std::memory_order_relaxed);
	}
	myCookDtMs.store(dt_ms, std::memory_order_relaxed);    // outputBandwidth() reads it (Info callbacks)

	// The FIFO/window length for this cook, derived from the parameters and the sample rate by the
	// shared model in RateModel.h (the same function the bench and the tests use, so the CHOP and
	// the offline measurements cannot disagree about what a given setting means).
	const int win_samples = windowSamplesFrom(p, mySampleRate);
	myCapacity = static_cast<size_t>(win_samples);

	// --- 2. ingest ---
	// How many spectra to produce. Two independent caps apply and the smaller wins: what the input
	// and the Channel Mode allow (analysisChannelCount), and what the output was declared to hold.
	// The second is not redundant - TouchDesigner allocates the output from getOutputInfo, and
	// writing a channel that was never declared would corrupt memory rather than produce a channel.
	myExecStage = 2;
	const int num_channels = std::min(analysisChannelCount(cinput, p.chanMode), std::max(1, output->numChannels));
	myAnalysisChannels = num_channels;
	ingest(cinput, p.chanMode, num_channels, p, myCapacity);

	// --- 3. analysis job ---
	myExecStage = 3;
	// Start or stop the worker as the Async parameter changes. Both calls are idempotent, so this
	// compare-and-switch runs on every cook without a state flag of its own; stopping joins the
	// thread, which is why turning Async off mid-cook is a safe, synchronous operation.
	myAsync->configure(p.async, p.workerWake, p.workerPriority);   // no-op unless the mode changed
	myAsyncActive = myAsync->async();

	// Every cook publishes a job. With Async on the worker picks it up (poll or signal, see WorkerWake);
	// with Async off publish() runs the analysis right here, so this cook shows its own samples.
	fillJob(myAsync->jobSlot(), num_channels, win_samples, dt_ms);
	myAsync->publish();

	// --- 4. output (hold the previous spectrum when nothing new has been published) ---
	// Note it asks for the NEWEST published result, which in async mode is usually the previous
	// cook's job. See the header comment above for why that latency is the design.
	myExecStage = 4;
	copyResultsToOutput(output, num_channels);

	// --- 5. deferred Textport log (lock-free check; only locks when something was logged) ---
	// The plan log can be written from the worker thread, and a non-cook thread must never call into
	// Python, so log lines are buffered there and printed here. hasPending() is a relaxed atomic
	// read, so the steady state (nothing logged) costs one load and no lock.
	if (myLog.hasPending()) myLog.flushToTextport();
	myLastCookUs = usSince(t_start);
	myExecStage = 0;                                      // no step in progress; an error here means "outside the cook"
}

// The cook entry point TouchDesigner calls. Its entire job is to make sure that no exception can
// cross the DLL boundary: an exception escaping into TouchDesigner's own code would be undefined
// behaviour (and in practice a crash), so everything is caught here and turned into an error string
// plus a zeroed output. The actual cook is executeImpl() above.
//
// THE SUCCESS PATH ALSO DOES ONE THING, and it is worth knowing about because it is easy to delete
// by accident: it clears a previously latched error. See the comment inside.
void
FFT::execute(CHOP_Output* output, const OP_Inputs* inputs, void* reserved)
{
	try {
		executeImpl(output, inputs);
		// A CPU without AVX2/FMA "completes" every cook by outputting silence on purpose; that is the
		// error state, not a recovery from it, so the message set in the constructor must stay (it used
		// to be cleared on the first cook, leaving a silent node with no error badge).
		if (!myCpuOk) return;
		// The cook completed, so whatever the previous cook failed on is no longer this node's state, and
		// the error string is cleared here. It used to survive until the user happened to pulse Reset,
		// which is a latch with no relation to the fault: a node cooking perfectly well stayed flagged as
		// broken for as long as nobody pressed anything. That matters beyond the flag itself, because
		// TouchDesigner reports a node in an error state in place of the operator's own information, so a
		// latched error is one of the few things that empties a middle-click popup while every callback
		// behind it is running normally. A fault that recurs re-latches this on the next cook, so clearing
		// it hides nothing - it only stops the node from claiming a fault it no longer has.
		if (!myErrorText.empty()) {
			myLog.log("[FFT Plugin] recovered: this cook completed, clearing the latched error \"" + myErrorText + "\"");
			myErrorText.clear();
		}
	} catch (const std::exception& e) {
		myErrorText = std::string("exception at stage ") + std::to_string(myExecStage) + ": " + e.what();
		myLog.log("[FFT Plugin] ERROR: " + myErrorText + " — zeroing output");
		zeroOutputSafe(output);
	} catch (...) {
		myErrorText = "unknown exception at stage " + std::to_string(myExecStage);
		myLog.log("[FFT Plugin] ERROR: " + myErrorText + " — zeroing output");
		zeroOutputSafe(output);
	}
}

// =============================================================================================
// Info CHOP / DAT / popup / diagnostics (UI callbacks; not part of the real-time path)
// =============================================================================================
//
// These callbacks are *called* from inside a cook, so they are the real-time path in the only sense that
// matters here: every microsecond they spend is a microsecond added to the node's cook time, on the same
// thread that just ran the analysis. TouchDesigner drives them one item at a time - 21 Info CHOP channels,
// then ~276 Info DAT rows, then the popup - and each one asks for the same handful of numbers. So the
// shape of every accessor below is "pay once per cook, then hand out references", and the reason is
// measured rather than stylistic: the by-value version of statusSnapshot() took the mutex and copied two
// std::string members on every one of those ~300 calls, which is ~850 heap allocations per frame. That is
// what made a middle-click query take a frame or two to answer - and, because the same mutexes are held
// by the audio worker around a plan-log burst, what made it need several attempts.
const AnalysisPipeline::Status&
FFT::statusSnapshot()
{
	// Fast path: the published copy has not moved since this thread last copied it, so the previous copy is
	// still current. One acquire load, no lock, no allocation. The published copy changes only when the
	// pipeline rebuilds its plan or its tables, which is exactly when myStatusPubVersion moves.
	//
	// myStatusReadValid is what covers the window before the first publish, when both version counters are
	// still 0 and an equality test would wrongly say "already current" - leaving the popup reporting an empty
	// engine, N = 0 and 0 magnitude bins until the first plan landed. Cheap once, wrong every frame until then.
	myAsync->copyStatusIfNewer(myStatusReadVersion, myStatusReadValid, myStatusRead);
	return myStatusRead;
}

int32_t
FFT::getNumInfoCHOPChans(void* reserved1)
{
	// Counted, not just returned: this callback is the first stop in the info chain, so whether it is
	// entered at all is what separates "TD is not calling the chain" from "the text is not rendering".
	myInfoChopChansCalls.fetch_add(1, std::memory_order_relaxed);
	// First callback of the chain in every cook: refresh the cached Info DAT rows and popup text here
	// (a timestamp compare in the steady state - see renderInfoCache).
	renderInfoCache(false);
	return 36;
}

// One Info CHOP channel, by index. THE COUNT IS A CONTRACT: this switch must cover every index below
// getNumInfoCHOPChans()'s return value (36), and the positions must not move - a CHOP that references a
// channel by index would silently follow a reordering. New channels are appended at the end.
//
//   0 execute_count        how many times execute() has run    |  18 hz_per_sample        mean Hz between output bins
//   1 fft_size             N of the transform                  |  19 output_bandwidth_sps bins x frames per second
//   2 window_samples       samples in the analysis window      |  20 channel_fanout       1 when channels were parallelised
//   3 input_sample_rate    rate of the incoming audio          |  21 output_bins          the declared output width
//   4 output_sample_rate   bins x me.time.rate (throughput)    |  22 kaiser_beta          beta the window uses (0 = not Kaiser)
//   5 peak_freq_hz         channel 0 spectral peak             |  23 pickup_p50_us        publish -> worker pickup, median
//   6 peak_magnitude       magnitude at that peak              |  24 pickup_p99_us        ... 99th percentile
//   7 simd_avx2_active     1 when the AVX2 path is usable      |  25 pickup_late          pickups later than one frame
//   8 async_active         1 when the worker thread is on      |  26 analysis_latency_ms  window centre + async frame + pickup
//   9 cook_time_us         the whole cook, microseconds        |  27 aggregated_bins      output bins aggregating >= 2 FFT bins
//  10 dsp_time_us          the analysis alone                  |  28-35 feature_*         Spectral Features (0 when off):
//  11 linear_bins          FFT bins before the warp            |      centroid_hz, rolloff_hz, flatness, flux,
//  12 param_fetch_us       time spent reading parameters       |      rms_db, bass_db, mid_db, high_db
//  13 param_reads          number of parameter lookups         |
//  14 jobs_dropped         async jobs overwritten untaken      |
//  15 analysis_channels    spectra produced per cook           |
//  16 hold_frames          cooks behind the analysis is        |
//  17 linear_grid          output grid == linear FFT grid      |
void
FFT::getInfoCHOPChan(int index, OP_InfoCHOPChan* chan, void* reserved1)
{
	const AnalysisPipeline::Status& s = statusSnapshot();
	const FFTDSP::SpectralFeatures& f = myFeatures;
	const bool fOn = myHaveFeatures;
	auto set = [&](const char* name, double v) { chan->name->setString(name); chan->value = static_cast<float>(v); };
	switch (index) {
	case 0:  set("execute_count", myExecuteCount); break;
	case 1:  set("fft_size", static_cast<double>(s.fftSize)); break;
	case 2:  set("window_samples", static_cast<double>(s.capacity)); break;
	case 3:  set("input_sample_rate", mySampleRate); break;
	case 4:  set("output_sample_rate", outputSampleRate(myParams)); break;
	case 5:  set("peak_freq_hz", myPeakFrequencyHz); break;
	case 6:  set("peak_magnitude", myPeakMagnitude); break;
	case 7:  set("simd_avx2_active", myCpuOk ? 1.0 : 0.0); break;
	case 8:  set("async_active", myAsyncActive ? 1.0 : 0.0); break;
	case 9:  set("cook_time_us", myLastCookUs); break;
	case 10: set("dsp_time_us", myAsync->dspUs()); break;
	case 11: set("linear_bins", static_cast<double>(s.linearBins)); break;
	case 12: set("param_fetch_us", myParamUs); break;
	case 13: set("param_reads", myParamReads); break;
	case 14: set("jobs_dropped", static_cast<double>(myAsync->jobsDropped())); break;
	case 15: set("analysis_channels", myAnalysisChannels); break;
	case 16: set("hold_frames", myHoldFrames); break;
	// 17: the output grid IS the linear FFT grid (the warp came out as the identity), read off the tables.
	case 17: set("linear_grid", s.linearGrid ? 1.0 : 0.0); break;
	case 18: set("hz_per_sample", hzPerSample(myParams, mySampleRate)); break;
	case 19: set("output_bandwidth_sps", outputBandwidth(myParams)); break;
	case 20: set("channel_fanout", myAsync->parallelActive() ? 1.0 : 0.0); break;
	case 21: set("output_bins", myOutputBins); break;
	case 22: set("kaiser_beta", s.kaiserBeta); break;
	case 23: set("pickup_p50_us", myAsyncActive ? myAsync->pickup().p50Us : 0.0); break;
	case 24: set("pickup_p99_us", myAsyncActive ? myAsync->pickup().p99Us : 0.0); break;
	case 25: set("pickup_late", static_cast<double>(myAsync->pickup().late)); break;
	case 26: {
		// What the spectrum lags the audio by: the window's centre (half a window of history), plus one
		// frame when the analysis runs on the worker (the cook shows the previous job's result), plus the
		// worker's median pickup. The number to compensate audio-reactive visuals against.
		const double win_ms = s.capacity > 0 && mySampleRate > 0 ? 0.5 * static_cast<double>(s.capacity) / mySampleRate * 1000.0 : 0.0;
		const double async_ms = myAsyncActive ? myCookDtMs.load(std::memory_order_relaxed) + myAsync->pickup().p50Us / 1000.0 : 0.0;
		set("analysis_latency_ms", win_ms + async_ms);
		break;
	}
	case 27: set("aggregated_bins", static_cast<double>(s.aggregatedBins)); break;
	case 28: set("feature_centroid_hz", fOn ? f.centroidHz : 0.0); break;
	case 29: set("feature_rolloff_hz", fOn ? f.rolloffHz : 0.0); break;
	case 30: set("feature_flatness", fOn ? f.flatness : 0.0); break;
	case 31: set("feature_flux", fOn ? f.flux : 0.0); break;
	case 32: set("feature_rms_db", fOn ? f.rmsDb : 0.0); break;
	case 33: set("feature_bass_db", fOn ? f.bassDb : 0.0); break;
	case 34: set("feature_mid_db", fOn ? f.midDb : 0.0); break;
	case 35: set("feature_high_db", fOn ? f.highDb : 0.0); break;
	default: break;
	}
}

// How many plan-log rows the Info DAT shows (the newest ones). The log keeps up to 256 entries; every row
// shown is one more getInfoDATEntries call per cook, so the DAT shows the recent history and the textport
// keeps the rest.
static constexpr size_t kInfoDatLogRows = 32;

bool
FFT::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	myInfoDatSizeCalls.fetch_add(1, std::memory_order_relaxed);
	// The row count and the rows themselves must come from the SAME view of the log (PlanLog::log()
	// truncates at its cap, so the log can shrink between two reads). Freeze it here, re-taken only when
	// the log actually changed, and keep only the newest kInfoDatLogRows entries.
	const uint64_t logVer = myLog.version();
	if (logVer != myInfoDatLogVersion) {
		myInfoDatLog = myLog.snapshot();
		if (myInfoDatLog.size() > kInfoDatLogRows)
			myInfoDatLog.erase(myInfoDatLog.begin(), myInfoDatLog.end() - static_cast<std::ptrdiff_t>(kInfoDatLogRows));
		myInfoDatLogVersion = logVer;
	}
	infoSize->rows = kInfoFixedRows + static_cast<int32_t>(myInfoDatLog.size());
	infoSize->cols = 2;
	infoSize->byColumn = false;
	return true;
}

// One Info DAT row. The fixed rows were formatted by renderInfoCache() (at most every kInfoRenderMs or
// when the status / log changed); this only hands the cached strings over.
//
//   0 execute_count          8 window_resolution      16 hz_per_sample          22 resolution
//   1 mode                   9 spectral_peak_freq     17 output_bandwidth_sps   23+ plan_log_* (newest 32)
//   2 fft_size              10 simd_acceleration      18 channel_fanout
//   3 linear_bins           11 fft_engine             19 info_callback_calls  <- read this one first
//   4 window_samples        12 cook_time              20 worker_pickup
//   5 input_sample_rate     13 dsp_time               21 analysis_latency
//   6 output_spectrum_axis  14 async_jobs
//   7 output_sample_rate    15 fft_backend
void
FFT::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1)
{
	if (index >= 0 && index < kInfoFixedRows) {
		entries->values[0]->setString(myDatName[index].c_str());
		entries->values[1]->setString(myDatValue[index].c_str());
		return;
	}
	const size_t log_idx = static_cast<size_t>(index - kInfoFixedRows);
	if (log_idx < myInfoDatLog.size()) {
		char name[32];
		snprintf(name, sizeof(name), "plan_log_%zu", log_idx);
		entries->values[0]->setString(name);
		entries->values[1]->setString(myInfoDatLog[log_idx].c_str());
	}
}

// Formats the fixed Info DAT rows and the popup text into member strings. force = true re-renders now;
// otherwise it re-renders when kInfoRenderMs elapsed or the status / log version moved. std::string
// assignment reuses each string's capacity, so once warm the refresh itself allocates (almost) nothing,
// and between refreshes the info callbacks do no formatting at all.
void
FFT::renderInfoCache(bool force)
{
	const auto now = std::chrono::steady_clock::now();
	const AnalysisPipeline::Status& s = statusSnapshot();
	const uint64_t statusVer = myStatusReadVersion;
	const uint64_t logVer = myLog.version();
	const bool due = force || myInfoRenderedAt.time_since_epoch().count() == 0 ||
	                 std::chrono::duration<double, std::milli>(now - myInfoRenderedAt).count() >= kInfoRenderMs ||
	                 statusVer != myInfoRenderedStatus || logVer != myInfoRenderedLog;
	if (!due) return;
	myInfoRenderedAt = now;
	myInfoRenderedStatus = statusVer;
	myInfoRenderedLog = logVer;

	char b[320];
	const double dspUs = myAsync->dspUs();
	const AsyncAnalysis::Pickup pk = myAsync->pickup();
	const Parameters::Values& p = myParams;
	int r = 0;
	auto row = [&](const char* name, const char* value) {
		if (r >= kInfoFixedRows) return;
		myDatName[r] = name;
		myDatValue[r] = value;
		++r;
	};
	snprintf(b, sizeof(b), "%d", myExecuteCount); row("execute_count", b);
	snprintf(b, sizeof(b), "%s, %d analysis channel(s)", myAsyncActive ? "async (worker thread)" : "sync (cook thread)", myAnalysisChannels); row("mode", b);
	snprintf(b, sizeof(b), "%zu", s.fftSize); row("fft_size", b);
	snprintf(b, sizeof(b), "%zu of %zu computed", s.magnitudeBins, s.linearBins); row("linear_bins", b);
	snprintf(b, sizeof(b), "%zu", s.capacity); row("window_samples", b);
	snprintf(b, sizeof(b), "%.1f Hz", mySampleRate); row("input_sample_rate", b);
	{
		// The frequency axis itself (axisBottom .. axisRate/2, last bin on the top). This - not the
		// sample rate - converts a bin index to Hz. The low end is printed, not assumed to be DC.
		const double axis_rate = outputAxisRate(p, mySampleRate);
		snprintf(b, sizeof(b), "%.2f Hz per bin x %d bins = %.1f..%.1f Hz", hzPerSample(p, mySampleRate), myOutputBins, s.axisBottom, axis_rate * 0.5);
		row("output_spectrum_axis", b);
	}
	{
		const double rate = myCookRate.load(std::memory_order_relaxed);
		snprintf(b, sizeof(b), "%.0f samples/s (%d bins x %.2f frames/s, %s)", outputSampleRate(p), myOutputBins,
		         rate > 0.0 ? rate : 60.0, s.linearGrid ? "linear grid, no resampling" : "warped grid, resampled");
		row("output_sample_rate", b);
	}
	snprintf(b, sizeof(b), "%.2f Hz (%zu-sample window)", s.capacity > 0 ? mySampleRate / static_cast<double>(s.capacity) : 0.0, s.capacity); row("window_resolution", b);
	snprintf(b, sizeof(b), "%.1f Hz", myPeakFrequencyHz); row("spectral_peak_freq", b);
	row("simd_acceleration", myCpuOk ? "AVX2 256-bit FMA" : "UNSUPPORTED CPU (no AVX2)");
	myDatName[r] = "fft_engine"; myDatValue[r] = s.plan; if (s.planUpgrading) myDatValue[r] += " [measuring better plan in background]"; ++r;
	snprintf(b, sizeof(b), "cook %.1f us (params %.1f us / %d reads this cook)", myLastCookUs, myParamUs, myParamReads); row("cook_time", b);
	snprintf(b, sizeof(b), "%.1f us per analysis (%s%s)", dspUs, myAsyncActive ? "off the cook thread" : "on the cook thread",
	         myAsync->parallelActive() ? ", channel loop parallel" : ""); row("dsp_time", b);
	snprintf(b, sizeof(b), "%llu dropped, hold %d frame(s)", static_cast<unsigned long long>(myAsync->jobsDropped()), myHoldFrames); row("async_jobs", b);
	{
		// Which library is loaded, and where its wisdom lives (per backend; resolved once per backend).
		const FFTDSP::FftBackendInfo& be = myAsync->backendInfo();
		if (myWisdomPathFor != &be) { myWisdomPathShown = FFTDSP::FFTWEngine::wisdomPathFor(be); myWisdomPathFor = &be; }
		myDatName[r] = "fft_backend";
		myDatValue[r] = std::string(be.display) + " | wisdom: " + myWisdomPathShown;
		if (!s.backend.empty()) myDatValue[r] += " | " + s.backend;
		++r;
	}
	{
		const bool uniform = (p.scale == Parameters::Scale::Linear) || (p.warp <= 0.0);
		snprintf(b, sizeof(b), "%.4f Hz per bin%s", hzPerSample(p, mySampleRate), uniform ? "" : " (mean; a perceptual grid is not uniform)");
		row("hz_per_sample", b);
	}
	{
		const double dt_ms = myCookDtMs.load(std::memory_order_relaxed);
		snprintf(b, sizeof(b), "%.0f samples/s (%d bins x %.1f frames/s)", outputBandwidth(p), myOutputBins, dt_ms > 0.0 ? 1000.0 / dt_ms : 0.0);
		row("output_bandwidth_sps", b);
	}
	row("channel_fanout", myAsync->parallelActive() ? "on: std::execution::par over the channel loop"
	                                                : "off: one channel, nothing to fan out (expected - this node is mono per instance)");
	// The diagnostic row (see README, "The middle-click info popup"): counters frozen while the node visibly
	// cooks = the chain is not entered; counters climbing with an ordinary popup length = a real string was
	// handed over and not drawn. Refreshed with the rest of the cache (<= 4 Hz), which is plenty for that.
	snprintf(b, sizeof(b), "popup %u (%u chars), Info CHOP %u, Info DAT %u | largest cook gap %.3f ms",
	         myInfoPopupCalls.load(std::memory_order_relaxed), myInfoPopupLen.load(std::memory_order_relaxed),
	         myInfoChopChansCalls.load(std::memory_order_relaxed), myInfoDatSizeCalls.load(std::memory_order_relaxed),
	         myMaxCookGapMs.load(std::memory_order_relaxed));
	row("info_callback_calls", b);
	if (myAsyncActive)
		snprintf(b, sizeof(b), "p50 %.0f us, p99 %.0f us, max %.0f us, %llu later than a frame (wake %s, priority %s)",
		         pk.p50Us, pk.p99Us, pk.maxUs, static_cast<unsigned long long>(pk.late),
		         p.workerWake == Parameters::WorkerWake::Signal ? "Signal" : "Poll",
		         p.workerPriority == Parameters::WorkerPriority::Mmcss ? "MMCSS Pro Audio" : "Highest");
	else
		snprintf(b, sizeof(b), "n/a (Async off: the analysis runs inside the cook)");
	row("worker_pickup", b);
	{
		const double win_ms = s.capacity > 0 && mySampleRate > 0 ? 0.5 * static_cast<double>(s.capacity) / mySampleRate * 1000.0 : 0.0;
		const double frame_ms = myAsyncActive ? myCookDtMs.load(std::memory_order_relaxed) : 0.0;
		snprintf(b, sizeof(b), "%.1f ms = window centre %.1f + async frame %.1f + pickup %.2f", win_ms + frame_ms + (myAsyncActive ? pk.p50Us / 1000.0 : 0.0),
		         win_ms, frame_ms, myAsyncActive ? pk.p50Us / 1000.0 : 0.0);
		row("analysis_latency", b);
	}
	{
		static const char* aggNames[] = { "off (interpolate)", "peak", "rms" };
		static const char* presetNames[] = { "Custom", "Visual 60", "Visual 120", "Analysis" };
		snprintf(b, sizeof(b), "%d output bins (%s, fft %zu%s), kaiser beta %.2f (%s), aggregation %s over %zu bins, preset %s, ingest %s",
		         myOutputBins, p.rawBins ? "Raw rfft" : (p.binsMode == Parameters::BinsMode::Auto ? "Auto = N/2+1" : "Fixed"),
		         s.fftSize, p.zeroPad ? " zero-padded" : " no padding", s.kaiserBeta,
		         p.betaMode == Parameters::BetaMode::Auto ? "Auto" : "Manual", aggNames[std::clamp(s.aggregation, 0, 2)], s.aggregatedBins,
		         presetNames[std::clamp(static_cast<int>(p.preset), 0, 3)], p.ingestMode == Parameters::IngestMode::Auto ? "Auto" : "Append All");
		row("resolution", b);
	}
	while (r < kInfoFixedRows) { myDatName[r] = "reserved"; myDatValue[r].clear(); ++r; }

	// ---- the popup text ----
	// POPUP STRING RULES (see README, "The middle-click info popup"): never add diagnostics beyond the
	// fixed lines below, keep the total under kMaxPopupChars, build it whole and hand it over with ONE
	// setString (in getInfoPopupString). The identity block comes first and is never dropped.
	const char* nodePath = (myNodeInfo && myNodeInfo->opPath) ? myNodeInfo->opPath : "<unknown>";
	const char* dllPath = (myNodeInfo && myNodeInfo->pluginPath) ? myNodeInfo->pluginPath : "<unknown>";
	std::string& text = myPopupText;
	text.clear();
	text += "Node: "; text += nodePath; text += " ("; text += kOpType; text += ", CHOP)\n";
	snprintf(b, sizeof(b), "Plugin: TouchDesigner Custom FFT v%d.%d.%d\n", kMajorVersion, kMinorVersion, kPatchVersion); text += b;
	text += "Binary: "; text += dllPath; text += "\n";
	snprintf(b, sizeof(b), "Mode: %s, %d analysis channel(s)\n", myAsyncActive ? "async worker" : "sync", myAnalysisChannels); text += b;
	auto add = [&text](const char* line) {
		if (text.size() + std::strlen(line) <= kMaxPopupChars) text += line;
	};
	auto addStr = [&text](const std::string& line) {
		if (text.size() + line.size() <= kMaxPopupChars) text += line;
	};
	addStr("Engine: " + s.plan + "\n");
	snprintf(b, sizeof(b), "FFT: N=%zu, window %zu, %zu magnitude bins\n", s.fftSize, s.capacity, s.magnitudeBins); add(b);
	snprintf(b, sizeof(b), "Axis: %d bins @ %.2f Hz, %.1f-%.1f Hz (input %.1f Hz, %s)\n", myOutputBins, hzPerSample(p, mySampleRate),
	         s.axisBottom, outputAxisRate(p, mySampleRate) * 0.5, mySampleRate, s.linearGrid ? "linear grid, no resample" : "warped grid"); add(b);
	{
		const double dt_ms = myCookDtMs.load(std::memory_order_relaxed);
		const double rate = myCookRate.load(std::memory_order_relaxed);
		snprintf(b, sizeof(b), "Rate: %.0f Hz = %d bins x %.2f fps | measured %.0f samples/s (cook %.2f ms)\n",
		         outputSampleRate(p), myOutputBins, rate > 0.0 ? rate : 60.0, outputBandwidth(p), dt_ms); add(b);
	}
	snprintf(b, sizeof(b), "Output: %d x %d ch @ %.0f Hz\n", myOutputBins, myAnalysisChannels, outputSampleRate(p)); add(b);
	snprintf(b, sizeof(b), "Cook: %.1f us CPU (params %.1f us) | DSP: %.1f us\n", myLastCookUs, myParamUs, dspUs); add(b);
	add(myCpuOk ? "SIMD: AVX2 256-bit FMA | GPU: none (CPU only)\n" : "SIMD: UNSUPPORTED CPU | GPU: none (CPU only)\n");
	snprintf(b, sizeof(b), "--- Plan log (last %zu) ---\n", kTailPlanLogLines); add(b);
	myLog.snapshotTail(kTailPlanLogLines, myPopupTail);
	for (const std::string& line : myPopupTail) addStr(FFTDSP::clipLine(line, kMaxTailLineChars) + "\n");
}

void
FFT::getInfoPopupString(OP_String* info, void* reserved1)
{
	// Counted, not announced: the counter (Info DAT row info_callback_calls) answers "is TouchDesigner
	// calling this at all?", which the popup itself cannot.
	myInfoPopupCalls.fetch_add(1, std::memory_order_relaxed);
	if (myPopupText.empty()) renderInfoCache(true);
	// ONE setString per call, of text built whole (renderInfoCache): the shape this harness was observed
	// rendering. The text always starts with the identity block, so it is never textually empty.
	info->setString(myPopupText.c_str());
	myInfoPopupLen.store(static_cast<uint32_t>(myPopupText.size()), std::memory_order_relaxed);
}

// The node's warning: working but not well. hold_frames == 1 is the normal async latency; > 3 means the
// analysis is consistently behind. The remedy names the settings that reduce the work.
void
FFT::getWarningString(OP_String* warning, void* reserved1)
{
	if (myAsyncActive && myHoldFrames > 3) {
		warning->setString("Analysis worker is falling behind (holding the previous spectrum); reduce Zero-Pad Len or Output Bins, or use a Visual preset.");
	}
}

// The node's error. TouchDesigner reports an errored node's error INSTEAD of its operator information,
// so every source here is a live condition that clears itself (none is a lifetime latch):
//   1. myErrorText       - an exception a recent cook threw (cleared by the next completed cook), or the
//                          no-AVX2 CPU message (kept: that condition never goes away)
//   2. planFailed        - no usable FFT plan for the current size
//   3. pipelineFailing   - the most recent analysis threw
//   4. the input reports a sample rate <= 0 (the node analyses at 44.1 kHz meanwhile)
void
FFT::getErrorString(OP_String* error, void* reserved1)
{
	if (!myErrorText.empty()) { error->setString(myErrorText.c_str()); return; }
	if (myAsync->planFailed()) { error->setString("FFTW plan creation failed for the current FFT size (see textport log); output is silent until a plan succeeds or the FFT size changes."); return; }
	if (myAsync->pipelineFailing()) {
		const std::string msg = std::to_string(myAsync->pipelineErrors()) + " analysis pipeline error(s), the most recent on the last analysis; previous spectrum retained, see textport log.";
		error->setString(msg.c_str());
		return;
	}
	if (!(myRawInputRate > 0.0)) error->setString("The input CHOP reports no usable sample rate (<= 0); analysing as 44100 Hz.");
}

// Called once, when TouchDesigner builds the node's parameter page. The whole parameter set lives in
// Parameters.h/.cpp - add a parameter there, not here.
void
FFT::setupParameters(OP_ParameterManager* manager, void* reserved1)
{
	Parameters::setup(manager);
}

void
FFT::setParameterEnableStates(const OP_Inputs* inputs, OP_ParEnableState* state, void* reserved1)
{
	Parameters::setEnableStates(inputs, state);
}

// Reset: forget everything that has history (EQ state, ballistics/AGC, the ingest position), without
// rebuilding the plan or the tables. It only sets flags the next cook acts on, because the pipeline state
// belongs to its owner thread.
void
FFT::pulsePressed(const char* name, void* reserved1)
{
	if (!strcmp(name, Parameters::ResetName))
	{
		for (auto& st : myIngest) st.eq.reset();
		myIngestCursor.reset();
		myResetPending = true;
		// An explicit Reset acknowledges a previous exception - but not the CPU check, which is a fact.
		if (myCpuOk) myErrorText.clear();
	}
}
