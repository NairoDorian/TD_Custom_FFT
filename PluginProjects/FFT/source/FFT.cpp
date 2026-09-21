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
// the popup lie about which build is loaded, which is exactly what happened for v2.9.0.
const int   kMajorVersion = 2;
const int   kMinorVersion = 9;   // keep in step with CHANGELOG.md - the v2.9.0 work landed without this bump,
                                 // so the popup and the Info DAT reported v2.8 for a v2.9 node
// Reported in the popup's `Plugin:` line, which is the only place in TouchDesigner that says which build is
// answering. That matters here more than usual: the popup is the surface that goes blank, so the first thing
// to establish when it comes back is *which* DLL produced it - the `Binary:` line gives the path, and this
// gives the build. It is also how a fix to the popup's own text can be confirmed as loaded at a glance.
const int   kPatchVersion = 1;

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
#include "RateModel.h"

#ifdef _WIN32
// Windows-only, and called on the worker thread as its first action. Two jobs:
//   1. raise the thread's priority so a busy machine cannot delay a result into the next frame
//   2. set a debugger-visible name so the thread is identifiable in a profiler or in Visual Studio's
//      Threads window. SetThreadDescription is resolved at runtime rather than linked because it
//      only exists from Windows 10 1607 onwards; older systems simply get no name.
// The priority reasoning is in the comment below and is worth reading before changing the level.
void nameAndBoostCurrentThread(const wchar_t* name)
{
	// THREAD_PRIORITY_HIGHEST (normal + 2): the worker now sits above TouchDesigner's normal-priority
	// threads — including the cook thread that hands it the next job — so a busy machine cannot delay
	// a result into the next frame (which shows up as hold_frames > 1). It sleeps > 99 % of the time
	// and does ~50 us of work per frame, so the preemption window it can open is small; the cost is
	// that when it DOES run long (a first-time measured plan, a large FFT), it no longer yields to
	// whatever TD is doing. THREAD_PRIORITY_TIME_CRITICAL is deliberately not used: that level can
	// starve the audio and UI threads, and buys nothing over HIGHEST for a 50 us burst.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
		if (auto fn = reinterpret_cast<SetDescFn>(GetProcAddress(k32, "SetThreadDescription"))) fn(GetCurrentThread(), name);
	}
}
#endif

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
}

// Destroying the operator stops the worker and joins it before the pipeline is destroyed (the
// member's own destructor runs after this body). That order matters: the worker thread calls
// into the pipeline, so the pipeline must outlive it.
FFT::~FFT()
{
	stopWorker();
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
	return axisRate(p, sampleRate, myOutputSampleRate.load(std::memory_order_relaxed));
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
	return sampleRateToTouchDesigner(p, myCookRate.load(std::memory_order_relaxed));
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
	return hzPerBin(p, sampleRate, myOutputSampleRate.load(std::memory_order_relaxed));
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
	return throughput(p, myCookDtMs.load(std::memory_order_relaxed));
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
	const int bins = outputBinCountFrom(p);
	info->startIndex = 0;
	info->numSamples = bins;
	info->numChannels = analysisChannelCount(cinput, p.chanMode);
	// Note this is the throughput reading, not the axis rate - see outputSampleRate() above and the
	// `output_spectrum_axis`/`hz_per_sample` Info rows for the frequency meaning of the bins.
	info->sampleRate = static_cast<float>(outputSampleRate(p));
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
	const size_t keep = std::min(n, capacity);                 // only samples that will still be inside the window
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

// Pipeline owner thread: worker (Async on) or cook thread (Async off)
// ---------------------------------------------------------------------------------------------
// WHAT THIS DOES: runs one job through the pipeline and publishes the result, then refreshes
// whatever of the node's cross-thread telemetry that result changed. It is the ONLY place the
// pipeline is called, and it is called by whichever thread owns the pipeline at the time.
//
// WHY IT IS WRITTEN AS "TRY, PUBLISH, THEN REPUBLISH TELEMETRY" RATHER THAN A PLAIN CALL:
//   * The pipeline is not allowed to throw into the worker thread - there is no cook above it to
//     catch it - so a throw is caught here, counted, and the previous result is left in place.
//     The slot is only published on success, which means the node keeps showing the last good
//     spectrum instead of blanking out.
//   * myPipelineFailing is set on a throw and cleared on the next success, so it always answers
//     "is the node failing right now" rather than "has it ever failed" - the lifetime count is
//     kept separately in myPipelineErrors for the Info DAT.
//   * The status strings are rebuilt only when the pipeline's status version moves, which is only
//     when the plan or the tables changed - not once per cook.
//
// THREADING: everything written here is either an atomic the info callbacks read (myDspUs,
// myOutputSampleRate, myPlanFailed, ...) or the status copy under myStatusMutex. The results buffer
// is the single-producer/single-consumer handoff described in DSPModules.h.
void
FFT::runJob(const AnalysisJob& job)
{
	AnalysisResult& res = myResults.back();
	try {
		myPipeline->process(job, res);
	} catch (...) {
		// The slot is not published: the previous result stays visible. Record that an analysis
		// failed so it surfaces instead of vanishing silently into the textport. Two records, because
		// they answer different questions: the counter is the lifetime tally for the Info DAT, and the
		// flag is whether the node is failing *now*, which is what the error string reports.
		myPipelineErrors.fetch_add(1, std::memory_order_relaxed);
		myPipelineFailing.store(true, std::memory_order_relaxed);
		myLog.log("[FFT Plugin] [pipeline] analysis threw an exception; previous spectrum retained");
		return;
	}
	myPlanFailed.store(myPipeline->planFailed(), std::memory_order_relaxed);
	res.seq = job.seq;
	myResults.publish();
	// A published result is the proof the pipeline is working again, so the failure flag goes down here.
	// If the fault is persistent it is set again by the very next analysis, so this cannot mask anything.
	myPipelineFailing.store(false, std::memory_order_relaxed);
	myDspUs.store(myPipeline->lastUs(), std::memory_order_relaxed);
	// Exactly the axis rate read off the tables that produced these bins — it is 2*(top of the axis)
	// by construction, never fitted and never assumed (relaxed: the cook only needs it to be a recent,
	// self-consistent value, and every channel of this cook reports the same one).
	myOutputSampleRate.store(myPipeline->outputSampleRate(), std::memory_order_relaxed);

	const uint64_t ver = myPipeline->statusVersion();
	if (ver != myStatusVersionSeen) {  // strings are built only when the plan / tables actually changed
		std::lock_guard<std::mutex> lock(myStatusMutex);
		myStatusCopy = myPipeline->status();
		myStatusVersionSeen = ver;
		// Release, and inside the lock: a reader that observes this version is guaranteed to see the
		// myStatusCopy written above it. Readers compare this instead of re-copying (see statusSnapshot).
		myStatusPubVersion.fetch_add(1, std::memory_order_release);
	}
}

// ---------------------------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------------------------
// WHAT THE WORKER IS FOR: with Async on, the analysis (the FFT and everything after it) runs on
// this thread instead of on the cook thread, so the node's cook costs only the ingest and the
// output copy. With Async off, neither of these is ever called and runJob() is invoked inline from
// execute() instead.
//
// LIFECYCLE: started and stopped from execute() (step 3) whenever the Async parameter differs from
// the current state, and stopped one final time from the destructor. stopWorker() joins the thread,
// so after it returns nothing can be running the pipeline - which is what makes it safe for the
// destructor to then destroy the pipeline the worker was calling into.
void
FFT::startWorker()
{
	if (myWorkerRunning) return;                       // idempotent: execute() calls this on every cook
	// Both flags are cleared BEFORE the thread starts, so the loop cannot immediately observe a
	// stop request left over from a previous run and exit at once.
	myWorkerStop.store(false, std::memory_order_release);
	myWorkerDormant.store(false, std::memory_order_release);
	myWorkerRunning = true;
	myWorker = std::thread([this]() { workerLoop(); });
}

void
FFT::stopWorker()
{
	if (!myWorkerRunning) return;                      // idempotent: also called from the destructor
	myWorkerStop.store(true, std::memory_order_release);
	// The signal is what wakes a DORMANT worker: in that state it is blocked in myWake.wait() and
	// would never see the stop flag on its own. A hot worker polls and sees the flag by itself.
	myWake.signal();
	if (myWorker.joinable()) myWorker.join();          // blocks until the loop has actually returned
	myWorkerRunning = false;
}

// The worker thread's whole body. It has exactly two modes and no other state:
//
//   HOT     - a job arrived recently, so the loop polls the slot every kWorkerPollMs. The cook
//             publishes a job without waking anything, which is why the poll exists: a kernel
//             wake-up costs ~5 us and the cook must never pay it in the normal case.
//   DORMANT - no job for kWorkerDormantAfterMs (TouchDesigner paused, or the node not cooking for
//             some other reason), so the loop blocks in myWake.wait() until the cook signals once.
//             This is the "node is idle" mode and it costs nothing at all.
//
// The dormancy transition is a Dekker handshake with the cook (store dormant; full fence; re-check
// the job slot) so a job published at that instant is never left unprocessed - one side or the
// other always sees the other's store, so the wake-up and the poll cannot both be missed.
//
// Call runJob() (below) to see what actually happens to a job once it is picked up.
void
FFT::workerLoop()
{
#ifdef _WIN32
	nameAndBoostCurrentThread(L"FFT Custom CHOP analysis");
#endif
	FFTDSP::DenormalGuard ftz;       // flush denormals for this thread; see the note in execute()
	auto lastJob = clk::now();
	for (;;) {
		if (myWorkerStop.load(std::memory_order_acquire)) return;
		if (myJobs.acquire()) {                          // latest job wins; older unconsumed jobs were overwritten
			runJob(myJobs.front());
			lastJob = clk::now();                        // this resets the dormancy countdown
			myWorkerDormant.store(false, std::memory_order_relaxed);
			continue;
		}
		if (!myWorkerDormant.load(std::memory_order_relaxed)) {
			if (std::chrono::duration<double, std::milli>(clk::now() - lastJob).count() > kWorkerDormantAfterMs) {
				// Going dormant: announce it with the strongest ordering, fence, then re-check the
				// slot on the next iteration rather than sleeping here. The fence is what pairs with
				// the cook's fence - see the handshake note above.
				myWorkerDormant.store(true, std::memory_order_seq_cst);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				continue;                                // re-check the slot before sleeping (pairs with the cook's fence)
			}
			myWake.waitFor(kWorkerPollMs);               // hot: short timed sleep, then poll again
		} else {
			myWake.wait();                               // dormant: block until the cook signals
		}
	}
}

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
	myResults.acquire();                                   // one atomic exchange; no-op when nothing new
	const AnalysisResult& res = myResults.front();
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
	const bool want_async = p.async;
	if (want_async != myWorkerRunning) {
		if (want_async) startWorker(); else stopWorker();
	}
	myAsyncActive = myWorkerRunning;

	// Every cook publishes a job. (v2.7.0 removed "Update Every N Cooks", which used to skip this on
	// N-1 cooks out of N: with the analysis already off the cook thread, skipping bought nothing and
	// only halved the rate at which the spectrum updated.)
	fillJob(myJobs.back(), num_channels, win_samples, dt_ms);
	// A publish that overwrites a job the worker never took is a dropped frame of analysis, counted
	// for the async_jobs Info DAT row. It is normal under load and not an error.
	if (myJobs.publish()) myJobsDropped.fetch_add(1, std::memory_order_relaxed);
	if (myAsyncActive) {
		// The hot worker polls the slot itself. Only a dormant worker needs a kernel wake-up
		// (~5 us): fence + load pair with the worker's store + fence, so exactly one side always
		// sees the other and the job is picked up either way.
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (myWorkerDormant.load(std::memory_order_seq_cst)) myWake.signal();
	} else if (myJobs.acquire()) {
		// Async off: the analysis runs right here, on the cook thread, and its result is published
		// before step 4 reads it - so a synchronous cook shows a spectrum computed from its own
		// samples, with no latency, at the cost of the transform being on the cook thread.
		runJob(myJobs.front());                           // inline on the cook thread
	}

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
	if (!myStatusReadValid || myStatusReadVersion != myStatusPubVersion.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> lock(myStatusMutex);
		myStatusRead = myStatusCopy;
		myStatusReadVersion = myStatusPubVersion.load(std::memory_order_relaxed);
		myStatusReadValid = true;
	}
	return myStatusRead;
}

int32_t
FFT::getNumInfoCHOPChans(void* reserved1)
{
	// Counted, not just returned: this callback is the first stop in the info chain, so whether it is
	// entered at all is what separates "TD is not calling the chain" from "the text is not rendering".
	// getInfoPopupString() reports the number in the popup itself.
	myInfoChopChansCalls.fetch_add(1, std::memory_order_relaxed);
	return 21;
}

// One Info CHOP channel, by index. TouchDesigner calls this once per channel, in order, right after
// getNumInfoCHOPChans() said how many there are.
//
// THE COUNT IS A CONTRACT: this switch must cover every index below `getNumInfoCHOPChans()`'s return
// value (21), and the names must stay in the same position, because a CHOP that exports or references
// a channel by index would silently follow a reordering. Adding a channel means: add a case here AND
// raise the 21 in getNumInfoCHOPChans(). Adding a case without raising it does nothing; raising it
// without adding a case hands TouchDesigner an unnamed zero-filled channel.
//
// READ THE INDEX MAP HERE, NOT IN TOUCHDESIGNER: the names are the API for anything downstream.
//
//   0 execute_count        how many times execute() has run  |  11 linear_bins        FFT bins before the warp
//   1 fft_size             N of the transform                |  12 param_fetch_us     time spent reading parameters
//   2 window_samples       samples in the analysis window    |  13 param_reads        number of parameter lookups
//   3 input_sample_rate    rate of the incoming audio        |  14 jobs_dropped       async jobs overwritten untaken
//   4 output_sample_rate   bins x me.time.rate (throughput) |  15 analysis_channels  spectra produced per cook
//   5 peak_freq_hz         channel 0 spectral peak           |  16 hold_frames        cooks behind the analysis is
//   6 peak_magnitude       magnitude at that peak            |  17 linear_grid        output grid == linear FFT grid
//   7 simd_avx2_active     1 when the AVX2 path is usable    |  18 hz_per_sample      Hz between adjacent output bins
//   8 async_active         1 when the worker thread is on    |  19 output_bandwidth_sps bins x frames per second
//   9 cook_time_us         the whole cook, microseconds      |  20 channel_fanout     1 when channels were parallelised
//  10 dsp_time_us          the analysis alone
//
// The read-only ones come from the status snapshot; see statusSnapshot() for why that matters.
void
FFT::getInfoCHOPChan(int index, OP_InfoCHOPChan* chan, void* reserved1)
{
	const AnalysisPipeline::Status& s = statusSnapshot();
	switch (index) {
	case 0:  chan->name->setString("execute_count");     chan->value = static_cast<float>(myExecuteCount); break;
	case 1:  chan->name->setString("fft_size");          chan->value = static_cast<float>(s.fftSize); break;
	case 2:  chan->name->setString("window_samples");    chan->value = static_cast<float>(s.capacity); break;
	case 3:  chan->name->setString("input_sample_rate"); chan->value = static_cast<float>(mySampleRate); break;
	case 4:  chan->name->setString("output_sample_rate");chan->value = static_cast<float>(outputSampleRate(myParams)); break;
	case 5:  chan->name->setString("peak_freq_hz");      chan->value = myPeakFrequencyHz; break;
	case 6:  chan->name->setString("peak_magnitude");    chan->value = myPeakMagnitude; break;
	case 7:  chan->name->setString("simd_avx2_active");  chan->value = myCpuOk ? 1.0f : 0.0f; break;
	case 8:  chan->name->setString("async_active");      chan->value = myAsyncActive ? 1.0f : 0.0f; break;
	case 9:  chan->name->setString("cook_time_us");      chan->value = static_cast<float>(myLastCookUs); break;
	case 10: chan->name->setString("dsp_time_us");       chan->value = static_cast<float>(myDspUs.load(std::memory_order_relaxed)); break;
	case 11: chan->name->setString("linear_bins");       chan->value = static_cast<float>(s.linearBins); break;
	case 12: chan->name->setString("param_fetch_us");    chan->value = static_cast<float>(myParamUs); break;
	case 13: chan->name->setString("param_reads");       chan->value = static_cast<float>(myParamReads); break;
	case 14: chan->name->setString("jobs_dropped");      chan->value = static_cast<float>(myJobsDropped.load(std::memory_order_relaxed)); break;
	case 15: chan->name->setString("analysis_channels"); chan->value = static_cast<float>(myAnalysisChannels); break;
	case 16: chan->name->setString("hold_frames");       chan->value = static_cast<float>(myHoldFrames); break;
	// 17: was `raw_linear`, driven by the removed Raw Linear Bins toggle. Same meaning, read off the
	// built tables instead of a parameter, and named for what it describes: the output grid IS the
	// linear FFT grid (the warp came out as the identity and the magnitude is memcpy'd).
	case 17: chan->name->setString("linear_grid");       chan->value = s.linearGrid ? 1.0f : 0.0f; break;
	case 18: chan->name->setString("hz_per_sample");     chan->value = static_cast<float>(hzPerSample(myParams, mySampleRate)); break;
	case 19: chan->name->setString("output_bandwidth_sps"); chan->value = static_cast<float>(outputBandwidth(myParams)); break;
	// 20: whether process() fanned the channel loop out over cores this cook. Same name as the Info DAT
	// row that reports it; off (0) is the normal reading for the intended one-mono-channel-per-node use.
	case 20: chan->name->setString("channel_fanout");    chan->value = (myPipeline && myPipeline->parallelActive()) ? 1.0f : 0.0f; break;
	}
}

bool
FFT::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	// Counted for the same reason as getNumInfoCHOPChans(): this runs immediately before
	// getInfoPopupString in the documented cook order, so if this number climbs and the popup's does
	// not, the chain is breaking between the two rather than never starting.
	myInfoDatSizeCalls.fetch_add(1, std::memory_order_relaxed);
	// The row count and the rows themselves must come from the SAME view of the log. PlanLog::log()
	// truncates the history by half once it reaches kMaxPlanLogEntries, so a plan event logged by the
	// worker between the two calls would make the log *shorter* after the size was declared - and every
	// row past the new end would then be left unwritten, handing TouchDesigner rows with nothing in
	// them. TouchDesigner only asks for the size once and then walks that many rows, so the fix is to
	// freeze the view here and have getInfoDATEntries() read exactly this copy. The info callbacks all
	// run on the cook thread, so the copy cannot change underneath the walk that follows it.
	//
	// The freeze is only re-taken when the log actually changed. That is the same guarantee - the copy is
	// still frozen for the whole walk - but it turns the steady state into an integer compare: this copy
	// is up to 256 std::strings, and it was being rebuilt on every cook whether or not anything had been
	// logged since the last one.
	const uint64_t logVer = myLog.version();
	if (logVer != myInfoDatLogVersion) {
		myInfoDatLog = myLog.snapshot();
		myInfoDatLogVersion = logVer;
	}
	infoSize->rows = 20 + static_cast<int32_t>(myInfoDatLog.size());
	infoSize->cols = 2;
	infoSize->byColumn = false;
	return true;
}

// One row of the Info DAT, by row index. Called once per row and per column pair, so it runs about
// twice per row for a two-column table - roughly 276 times per cook at the standard row count.
//
// THE INDEX MAP (the first 20 rows are fixed; rows 20+ are the plan log, one row per entry):
//
//   0 execute_count          10 simd_acceleration      20+ plan_log_0, plan_log_1, ...
//   1 mode                   11 fft_engine
//   2 fft_size               12 cook_time              <- the cook/param breakdown
//   3 linear_bins            13 dsp_time
//   4 window_samples         14 async_jobs             <- dropped count and hold frames
//   5 input_sample_rate      15 fft_backend            <- which library is loaded, and its wisdom
//   6 output_spectrum_axis   16 hz_per_sample
//   7 output_sample_rate     17 output_bandwidth_sps
//   8 window_resolution      18 channel_fanout
//   9 spectral_peak_freq     19 info_callback_calls    <- the diagnostic row; read this one first
//
// The rows are free-form strings rather than values: an Info DAT is read by a human, so each one
// carries its own units and, where a number could be misread, the arithmetic behind it.
//
// THE ROW COUNT MUST MATCH getInfoDATSize(). That function freezes the log view and declares
// `20 + rows`; this one walks the frozen copy. If you add a fixed row, change both.
void
FFT::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1)
{
	// By reference: this function runs once per row (~276 times per cook) and an earlier by-value version
	// copied two std::strings on each of those calls. See the block comment above statusSnapshot().
	const AnalysisPipeline::Status& s = statusSnapshot();
	const double dspUs = myDspUs.load(std::memory_order_relaxed);
	// One stack buffer reused by every branch below: the table is `byColumn = false` (see
	// getInfoDATSize), so values[0] is the row name and values[1] is the value, and setString copies
	// out of the buffer immediately. 256 characters is comfortably more than any row here produces.
	char tempBuffer[256];
	// The shortcut for the rows whose value is already a string. It exists to keep the switch below
	// readable: without it every string row would repeat the same two setString calls.
	auto row = [&](const char* k, const std::string& v) {
		entries->values[0]->setString(k);
		entries->values[1]->setString(v.c_str());
	};
	switch (index) {
	case 0: row("execute_count", std::to_string(myExecuteCount)); return;
	case 1: row("mode", std::string(myAsyncActive ? "async (worker thread)" : "sync (cook thread)") + ", " + std::to_string(myAnalysisChannels) + " analysis channel(s)"); return;
	case 2: row("fft_size", std::to_string(s.fftSize)); return;
	case 3: snprintf(tempBuffer, sizeof(tempBuffer), "%zu of %zu computed", s.magnitudeBins, s.linearBins); row("linear_bins", tempBuffer); return;
	case 4: row("window_samples", std::to_string(s.capacity)); return;
	case 5: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", mySampleRate); row("input_sample_rate", tempBuffer); return;
	case 6: {
		// The frequency axis itself: axisBottom .. outputAxisRate/2 with the last bin on the top, so
		// n_out bins cover (n_out-1) intervals of outputAxisRate/(2*(n_out-1)). This — not the sample
		// rate — is what converts a bin index to Hz. The low end is printed rather than assumed to be
		// DC: it is 0 only for Mel / ERB / Linear (and for any scale at Warp Blend 0).
		const double axis_rate = outputAxisRate(myParams, mySampleRate);
		const int n_out = outputBinCountFrom(myParams);
		snprintf(tempBuffer, sizeof(tempBuffer), "%.2f Hz per bin x %d bins = %.1f..%.1f Hz",
		         hzPerSample(myParams, mySampleRate), n_out, s.axisBottom, axis_rate * 0.5);
		row("output_spectrum_axis", tempBuffer);
		return;
	}
	case 7: {
		// bins x me.time.rate: the rate of one output vector per cook, i.e. the sample rate of the
		// frames concatenated. The axis row above carries the frequency meaning.
		const int n_out = outputBinCountFrom(myParams);
		const double rate = myCookRate.load(std::memory_order_relaxed);
		snprintf(tempBuffer, sizeof(tempBuffer), "%.0f samples/s (%d bins x %.2f frames/s, %s)",
		         outputSampleRate(myParams), n_out,
		         rate > 0.0 ? rate : 60.0,
		         s.linearGrid ? "linear grid, no resampling" : "warped grid, resampled");
		row("output_sample_rate", tempBuffer);
		return;
	}
	case 8: snprintf(tempBuffer, sizeof(tempBuffer), "%.2f Hz (%zu-sample window)", s.capacity > 0 ? mySampleRate / static_cast<double>(s.capacity) : 0.0, s.capacity);
	        row("window_resolution", tempBuffer); return;
	case 9: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", myPeakFrequencyHz); row("spectral_peak_freq", tempBuffer); return;
	case 10: row("simd_acceleration", myCpuOk ? "AVX2 256-bit FMA" : "UNSUPPORTED CPU (no AVX2)"); return;
	case 11: row("fft_engine", s.plan + (s.planUpgrading ? " [measuring better plan in background]" : "")); return;
	case 12: snprintf(tempBuffer, sizeof(tempBuffer), "cook %.1f us (params %.1f us / %d reads this cook)", myLastCookUs, myParamUs, myParamReads); row("cook_time", tempBuffer); return;
	case 13: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f us per analysis (%s%s)", dspUs,
	                  myAsyncActive ? "off the cook thread" : "on the cook thread",
	                  (myPipeline && myPipeline->parallelActive()) ? ", channel loop parallel" : ""); row("dsp_time", tempBuffer); return;
	case 14: snprintf(tempBuffer, sizeof(tempBuffer), "%llu dropped, hold %d frame(s)", static_cast<unsigned long long>(myJobsDropped.load(std::memory_order_relaxed)), myHoldFrames); row("async_jobs", tempBuffer); return;
	case 15: {
		// Which library is actually loaded, and where *its* wisdom lives. The two backends keep
		// separate wisdom files on purpose (a wisdom file names the library that wrote it, and FFTW
		// rejects one that does not match), so reporting a single hard-coded path would be wrong as
		// soon as the toggle is on. The live description comes from the status snapshot, not from the
		// pipeline directly: the snapshot is the one channel that is safe to read from the thread
		// TouchDesigner calls this on, while the engine's own state belongs to the worker thread.
		const FFTDSP::FftBackendInfo& be = myPipeline ? myPipeline->backendInfo() : FFTDSP::defaultBackend();
		std::string text = std::string(be.display) + " | wisdom: " +
		                   FFTDSP::FFTWEngine::wisdomPathFor(be);
		if (!s.backend.empty()) text += " | " + s.backend;
		row("fft_backend", text.c_str());
		return;
	}
	case 16: {
		// The axis is uniform whenever the blend collapses every scale onto the linear ramp: Scale =
		// Linear (its perceptual grid IS the linear one, so any blend stays uniform) or Warp Blend = 0
		// (the blend ignores the scale entirely). Otherwise the grid is perceptual and the number below
		// is the mean spacing, the only scalar that can describe a non-uniform axis.
		const bool uniform = (myParams.scale == Parameters::Scale::Linear) || (myParams.warp <= 0.0);
		snprintf(tempBuffer, sizeof(tempBuffer), "%.4f Hz per bin%s",
		         hzPerSample(myParams, mySampleRate),
		         uniform ? "" : " (mean; a perceptual grid is not uniform)");
		row("hz_per_sample", tempBuffer);
		return;
	}
	case 17: {
		// Throughput, not the sample rate: bins per new frame x frames per second. See outputBandwidth().
		const int n_out = outputBinCountFrom(myParams);
		const double dt_ms = myCookDtMs.load(std::memory_order_relaxed);
		snprintf(tempBuffer, sizeof(tempBuffer), "%.0f samples/s (%d bins x %.1f frames/s)",
		         outputBandwidth(myParams), n_out,
		         dt_ms > 0.0 ? 1000.0 / dt_ms : 0.0);
		row("output_bandwidth_sps", tempBuffer);
		return;
	}
	case 18: {
		const bool par = myPipeline && myPipeline->parallelActive();
		snprintf(tempBuffer, sizeof(tempBuffer), "%s",
		         par ? "on: std::execution::par over the channel loop"
		             : "off: one channel, nothing to fan out (expected - this node is mono per instance)");
		// Named for what it reports (whether the channel loop was fanned out), not for the parameters
		// that used to gate it - those are gone, and this row cannot disagree with what process() did.
		row("channel_fanout", tempBuffer);
		return;
	}
	case 19: {
		// The same counters the popup prints, readable without middle-clicking, plus the largest gap
		// between two cooks. Together they separate the three ways this node can look blank: the info
		// chain is not being entered at all (counters stuck at 0 while the node is visibly cooking), the
		// counters climb but nothing renders, or the node stopped cooking for a stretch (a stall far
		// above one frame). All three are invisible from outside, which is why they share a row.
		//
		// The popup's last character count rides here too, and here rather than in the popup string
		// precisely so it cannot be suspected of affecting what it measures: this row is read as a value,
		// not rendered as the popup, so whatever the popup does with its text cannot change this number.
		// Read it as: counters at 0 while the node cooks = the chain never reaches these callbacks; a
		// healthy character count on a blank popup = a real string was handed over and not rendered.
		//
		// The count is now a constant, which is what makes it readable at all. It used to drift by hundreds
		// of characters with whatever the engine last logged, so "the popup is blank and the length is 1400"
		// could not be compared against anything; the tail is clipped and the total bounded (see
		// getInfoPopupString), so the same node hands over the same number on every cook. A different
		// number therefore means one of the inputs changed - the node path, the DLL path, or a plan line -
		// and never that the popup outgrew something between one cook and the next.
		const double gap_ms = myMaxCookGapMs.load(std::memory_order_relaxed);
		row("info_callback_calls", "popup " + std::to_string(myInfoPopupCalls.load(std::memory_order_relaxed))
		    + " (" + std::to_string(myInfoPopupLen.load(std::memory_order_relaxed)) + " chars)"
		    + ", Info CHOP " + std::to_string(myInfoChopChansCalls.load(std::memory_order_relaxed))
		    + ", Info DAT " + std::to_string(myInfoDatSizeCalls.load(std::memory_order_relaxed))
		    + " | largest cook gap " + std::to_string(gap_ms) + " ms");
		return;
	}
	default: break;
	}
	// Past the fixed rows: one row per plan-log entry, in the order the frozen copy holds them.
	// Anything past the end of the frozen copy is left alone deliberately - getInfoDATSize() declared
	// the row count from that same copy, so this cannot normally be reached.
	size_t log_idx = static_cast<size_t>(index - 20);
	if (log_idx < myInfoDatLog.size()) {
		snprintf(tempBuffer, sizeof(tempBuffer), "plan_log_%zu", log_idx);
		row(tempBuffer, myInfoDatLog[log_idx]);
	}
}

void
FFT::getInfoPopupString(OP_String* info, void* reserved1)
{
	// Counted, not announced: the counter is the answer to "is TouchDesigner calling this callback at all?",
	// and it is read from the info_callback_calls Info DAT row. TouchDesigner calls the whole info chain
	// inside a cook, so an empty popup can mean either "TD never called us" (the chain broke before this
	// callback) or "TD called us and did not render the text" - opposite fixes, indistinguishable from the
	// popup itself, and this counter is what tells them apart. It is deliberately not printed to the
	// textport: this is the real-time path, and a diagnostic that prints on a cadence is noise in the
	// stream where the plan log lives.
	myInfoPopupCalls.fetch_add(1, std::memory_order_relaxed);

	// The text is built whole and handed to TouchDesigner exactly once, at the very end of this function,
	// on every path - see the note above the setString call for why the single call is load-bearing.
	// Everything between here and there only appends to `text` through `add` below.
	const char* nodePath = (myNodeInfo && myNodeInfo->opPath) ? myNodeInfo->opPath : "<unknown>";
	const char* dllPath  = (myNodeInfo && myNodeInfo->pluginPath) ? myNodeInfo->pluginPath : "<unknown>";
	// The identity block is built with += and never routed through the bound below: it is ~250 characters,
	// appended first, and it is the part that must survive whole - it names the node and the exact binary
	// answering for it, which is the first thing to check when a popup looks wrong (a stale second install
	// does exactly this; see README). Nothing below can reach it because the bound can only refuse appends
	// that would make the string longer, never shorten it.
	std::string text = std::string("Node: ") + nodePath + " (" + kOpType + ", CHOP)\n";
	text += "Plugin: TouchDesigner Custom FFT v" + std::to_string(kMajorVersion) + "." + std::to_string(kMinorVersion)
	      + "." + std::to_string(kPatchVersion) + "\n";
	text += std::string("Binary: ") + dllPath + "\n";
	text += std::string("Mode: ") + (myAsyncActive ? "async worker" : "sync") + ", " + std::to_string(myAnalysisChannels) + " analysis channel(s)\n";
	// Every append past the identity block goes through here, which is what makes kMaxPopupChars a bound on
	// the *string* rather than on one part of it. Whole lines only: a line that does not fit is dropped, not
	// clipped, because half a line of numbers reads as a bug in the numbers. Nothing here is load-bearing -
	// the identity block above already names the node and the binary - so dropping the tail under pressure
	// is the right trade, and at the current sizes (~700 characters typical against a 1200 bound) it does
	// not happen at all.
	auto add = [&text](const std::string& s) {
		if (text.size() + s.size() <= kMaxPopupChars) text += s;
	};
	try {
		const AnalysisPipeline::Status& s = statusSnapshot();
		char buf[256];
		add("Engine: " + s.plan + "\n");
		add("FFT: N=" + std::to_string(s.fftSize) + ", window " + std::to_string(s.capacity)
		    + ", " + std::to_string(s.magnitudeBins) + " magnitude bins\n");
		{
			const double axis_rate = outputAxisRate(myParams, mySampleRate);
			snprintf(buf, sizeof(buf), "%.2f Hz", hzPerSample(myParams, mySampleRate));
			std::string line = "Axis: " + std::to_string(outputBinCountFrom(myParams)) + " bins @ " + buf + ", ";
			snprintf(buf, sizeof(buf), "%.1f-%.1f Hz (input %.1f Hz, %s)\n",
			         s.axisBottom, axis_rate * 0.5, mySampleRate,
			         s.linearGrid ? "linear grid, no resample" : "warped grid");
			line += buf;
			add(line);
		}
		{
			// Rate and throughput share a line because they are the declared and the measured form of the same
			// quantity (see outputSampleRate / outputBandwidth), and reading them apart cost two lines of prose
			// for six numbers on a string that has to stay short. Both stay, with their two denominators, since
			// the whole point of reporting both is that they can differ: fps and the observed cook delta.
			const double dt_ms = myCookDtMs.load(std::memory_order_relaxed);
			const double rate = myCookRate.load(std::memory_order_relaxed);
			snprintf(buf, sizeof(buf), "Rate: %.0f Hz = %d bins x %.2f fps | measured %.0f samples/s (cook %.2f ms)\n",
			         outputSampleRate(myParams), outputBinCountFrom(myParams), rate > 0.0 ? rate : 60.0,
			         outputBandwidth(myParams), dt_ms);
			add(buf);
		}
		// The shape of what leaves the node, stated the way TouchDesigner sees it (samples per channel, channel
		// count, sample rate) - the same three numbers getOutputInfo() sets, so the popup and the node's output
		// can be compared without opening a CHOP viewer.
		snprintf(buf, sizeof(buf), "Output: %d x %d ch @ %.0f Hz\n",
		         outputBinCountFrom(myParams), myAnalysisChannels, outputSampleRate(myParams));
		add(buf);
		// One decimal, not std::to_string's six: std::to_string(double) is %f, so a cook time of 13 us printed
		// as "13.000000" - six digits of noise on a microsecond figure, and eleven wasted characters per number
		// on a string that has to stay short. The cook count that used to end this line is gone; proving the node
		// is cooking is the identity block's job, and the count is in the info_callback_calls Info DAT row where
		// reading it costs the popup nothing.
		snprintf(buf, sizeof(buf), "Cook: %.1f us CPU (params %.1f us) | DSP: %.1f us\n",
		         myLastCookUs, myParamUs, myDspUs.load(std::memory_order_relaxed));
		add(buf);
		// SIMD and GPU on one line, and the GPU half says only what is true of this node: it has no GPU stage,
		// so there is nothing for it to report. TouchDesigner's own Operator Info header carries the node's CPU
		// and GPU cook times for the frame; this line is the plugin's own measurement.
		add(std::string("SIMD: ") + (myCpuOk ? "AVX2 256-bit FMA" : "UNSUPPORTED CPU") + " | GPU: none (CPU only)\n");
		add("--- Plan log (last " + std::to_string(kTailPlanLogLines) + ") ---\n");
		// The tail is the only part of this string whose content is not fixed, and it used to be the only part
		// whose *length* was not fixed either: a plan line carries the absolute path of the FFT library and runs
		// to ~240 characters, so three of them moved the total by up to 700 depending on which events were last.
		// Each line is clipped here instead, which is what makes the popup's length predictable - the reason the
		// character count in the info_callback_calls row is worth reading at all is that it is now a constant.
		//
		// snapshotTail, not snapshot: only the last kTailPlanLogLines entries are ever shown, and snapshot()
		// copied the entire history - up to 256 strings and their allocations - to throw all but three away, on
		// every cook. The scratch vector is reused, so once it has reached its size this costs no allocation.
		myLog.snapshotTail(kTailPlanLogLines, myPopupTail);
		for (size_t i = 0; i < myPopupTail.size(); ++i) {
			add(FFTDSP::clipLine(myPopupTail[i], kMaxTailLineChars) + "\n");
		}
	} catch (const std::exception& e) {
		add(std::string("(telemetry truncated after the identity block: ") + e.what() + ")\n");
	} catch (...) {
		add("(telemetry truncated after the identity block: unknown exception)\n");
	}

	// ONE setString per call, at the end, reached on every path including both catches above. The
	// two-call version it replaced built a short identity block, published it, then published the full
	// text; the single-call form is what this harness was observed rendering, so it is what is kept.
	//
	// The safety property the two-call order was reaching for is kept without needing a second call:
	// `text` already holds the identity block before the try is entered, so if anything below throws, the
	// catches append a note rather than replacing it, and this call still hands TouchDesigner a populated
	// popup. It cannot come up empty because the callback ran and threw - only because TouchDesigner never
	// called it, which myInfoPopupCalls answers separately.
	info->setString(text.c_str());
	// The length actually handed over, reported through the info_callback_calls Info DAT row. It used to be a
	// variable - the tail could add up to ~700 characters depending on which plan events were last - and that
	// made it worthless as a signal: a blank popup alongside any length in that range said nothing. It is a
	// constant now (the body is fixed and every tail line is clipped), so the reading is unambiguous: an
	// ordinary length beside a blank popup means a real, well-formed string was handed over and not rendered,
	// while a frozen call count means the callback never ran at all.
	myInfoPopupLen.store(static_cast<uint32_t>(text.size()), std::memory_order_relaxed);

	// No textport announcement on entry any more. It existed to answer one question - "is TouchDesigner
	// calling this callback at all?" - and that question is now answered without printing anything: the
	// call count and the length handed over are both in the info_callback_calls Info DAT row, where they
	// are read as values on demand. The periodic line cost a buffered string per announcement and put
	// popup-traffic noise in the textport, which is where the plan log lives; a diagnostic belongs in the
	// DAT precisely so it cannot be mistaken for the thing it measures.
}

// The node's warning, shown on the operator itself. A warning means the node is working but not
// well, which is exactly the case the async worker falls into when it cannot keep up: the spectrum
// is still valid, it is just older than it should be.
//
// Setting no string (the normal case) is a no-op, not an empty warning.
void
FFT::getWarningString(OP_String* warning, void* reserved1)
{
	// hold_frames == 1 is the normal one-frame latency of the async pipeline - only a larger number
	// is worth mentioning, and 3 is the threshold that separates "a frame ran long" from "the
	// analysis is consistently behind". The remedy named in the message is the two settings that
	// actually reduce the work: a shorter zero-pad (smaller N) or fewer output bins.
	if (myAsyncActive && myHoldFrames > 3) {
		warning->setString("Analysis worker is falling behind (holding the previous spectrum); reduce Zero-Pad Len or Output Bins.");
	}
}

// The node's error, shown on the operator itself. TouchDesigner puts the node into an error state
// while this returns anything, and an errored node reports *that* instead of its own operator
// information - which is why a stale error is worth clearing aggressively (see execute()).
//
// FOUR SOURCES, checked in priority order: the most specific and most recent first.
//   1. myErrorText       - an exception this or a recent cook threw (carries the stage number)
//   2. myPlanFailed      - FFTW could not build a plan for the current size
//   3. myPipelineFailing - an analysis threw; live flag, not the lifetime count
//   4. mySampleRate <= 0 - the input gave no usable sample rate
// A node with none of these sets no string and is not in error.
void
FFT::getErrorString(OP_String* error, void* reserved1)
{
	if (!myErrorText.empty()) error->setString(myErrorText.c_str());
	else if (myPlanFailed.load(std::memory_order_relaxed)) error->setString("FFTW plan creation failed for the current FFT size (see textport log); output is silent until a plan succeeds or the FFT size changes.");
	// Gated on the live flag, not on the lifetime counter, and the count is reported as the history it is.
	// Driving this off "has an analysis ever thrown?" meant one transient exception - the kind a plugin
	// hot-swap during development produces - left the node erroring forever with the fault long gone.
	else if (myPipelineFailing.load(std::memory_order_relaxed)) error->setString((std::to_string(myPipelineErrors.load(std::memory_order_relaxed)) + " analysis pipeline error(s), the most recent on the last analysis; previous spectrum retained, see textport log.").c_str());
	else if (mySampleRate <= 0.0) error->setString("Invalid or missing audio sample rate from input CHOP.");
}

// Called once, when TouchDesigner builds the node's parameter page. The whole parameter set lives
// in Parameters.h/.cpp so that the CHOP's page and the values the pipeline reads are defined in one
// place - add a parameter there, not here.
void
FFT::setupParameters(OP_ParameterManager* manager, void* reserved1)
{
	Parameters::setup(manager);
}

// A pulse parameter was pressed. Only Reset does anything today.
//
// RESET MEANS "forget everything that has history" - the EQ's filter state and the ballistics/AGC
// state in the pipeline - so the node starts from a clean slate without rebuilding the plan or the
// tables. It is NOT a re-plan and NOT a reload; an empty middle-click popup is a different problem
// with a different answer (see README).
//
// The reset request is passed to the pipeline through the next job (myResetPending -> job.reset)
// rather than applied here, because the pipeline state belongs to whichever thread owns it.
void
FFT::pulsePressed(const char* name, void* reserved1)
{
	if (!strcmp(name, Parameters::ResetName))
	{
		for (auto& st : myIngest) st.eq.reset();
		// Also clears the latched error: after an explicit Reset the user has acknowledged whatever
		// was wrong, so the node should stop reporting it even if nothing else has changed.
		myResetPending = true;
		myErrorText.clear();
	}
}
