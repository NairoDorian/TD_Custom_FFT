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

bool g_cpuHasAVX2 = true;

const char* kOpType    = "Fftcustom";   // must not collide with the built-in FFT CHOP
const char* kOpLabel   = "FFT Custom";
const char* kOpIcon    = "FFT";
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
inline double usSince(clk::time_point t0) { return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); }

// The TD-free sample-rate / bin / axis model (windowSamplesFrom, fftSizeFrom,
// outputBinCountFrom, axisRate, sampleRateToTouchDesigner, hzPerBin, throughput) lives in
// RateModel.h so it is shared by the CHOP and the pipeline and is unit-testable headlessly.
#include "RateModel.h"

#ifdef _WIN32
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
	ginfo->inputMatchIndex = 0;
}

// ---------------------------------------------------------------------------------------------
// Parameters: one eval() per cook, from getOutputInfo() (TouchDesigner calls it right before
// execute()). execute() only re-polls if getOutputInfo() was skipped for this cook.
// ---------------------------------------------------------------------------------------------
void
FFT::pollParameters(const OP_Inputs* inputs)
{
	const auto t0 = clk::now();
	myParams = Parameters::eval(inputs, &myParamReads);
	myHaveParams = true;
	myParamUs = usSince(t0);
}

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
	info->sampleRate = static_cast<float>(outputSampleRate(p));
	return true;
}

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
void
FFT::ingest(const OP_CHOPInput* cinput, Parameters::ChanMode mode, int numChannels, const Parameters::Values& p, size_t capacity)
{
	if (static_cast<int>(myIngest.size()) != numChannels) myIngest.resize(static_cast<size_t>(numChannels));
	for (auto& st : myIngest) {
		if (st.fifo.capacity() != capacity) { st.fifo.resize(capacity); st.silent_run = 0; }
		st.eq.setSampleRate(mySampleRate);
	}
	if (!cinput || cinput->numChannels <= 0 || cinput->numSamples <= 0) return;

	const size_t n = static_cast<size_t>(cinput->numSamples);
	const size_t keep = std::min(n, capacity);                 // only samples that will still be inside the window
	const int inChans = cinput->numChannels;

	for (int ch = 0; ch < numChannels; ++ch) {
		IngestState& st = myIngest[static_cast<size_t>(ch)];
		if (st.block.size() < keep) st.block.resize(keep);
		float* blk = st.block.data();

		// --- source block ---
		if (mode == Parameters::ChanMode::MonoMix && inChans > 1) {
			const float* c0 = cinput->getChannelData(0);
			if (!c0) continue;
			std::memcpy(blk, c0 + (n - keep), keep * sizeof(float));
			for (int c = 1; c < inChans; ++c) {
				const float* cd = cinput->getChannelData(c);
				if (cd) FFTDSP::addInto(blk, cd + (n - keep), blk, keep);
			}
			FFTDSP::scaleInPlace(blk, keep, 1.0f / static_cast<float>(inChans));
		} else {
			const int src_ch = (mode == Parameters::ChanMode::AllChannels) ? std::min(ch, inChans - 1) : 0;
			const float* cd = cinput->getChannelData(src_ch);
			if (!cd) continue;
			std::memcpy(blk, cd + (n - keep), keep * sizeof(float));
		}

		// --- silence tracking (digital zero) ---
		if (FFTDSP::blockIsSilent(blk, keep)) st.silent_run = std::min(st.silent_run + keep, capacity * 2);
		else st.silent_run = 0;

		// --- EQ on the new samples only (stateful, time order) ---
		if (p.eqEnable && st.eq.updateAndCheckActive(p.gainDb, p.cutoffHz, p.lowGainDb, p.lowCutoffHz, p.q, p.amount)) {
			st.eq.processBlockInPlace(blk, keep, p.amount);
		}
		st.fifo.add(blk, keep);
	}
}

// ---------------------------------------------------------------------------------------------
// Job / result handoff
// ---------------------------------------------------------------------------------------------
void
FFT::fillJob(AnalysisJob& job, int numChannels, int winSamples, double dtMs)
{
	job.seq = ++myJobSeq;
	job.numChannels = numChannels;
	job.sampleRate = mySampleRate;
	job.winSamples = winSamples;
	job.dtMs = dtMs;
	job.p = myParams;
	job.reset = myResetPending;
	const size_t n = static_cast<size_t>(numChannels);
	if (job.windows.size() != n) job.windows.resize(n);     // no allocation after the first cooks
	if (job.silent.size() != n) job.silent.resize(n);
	for (int ch = 0; ch < numChannels; ++ch) {
		myIngest[ch].fifo.get(job.windows[ch]);
		job.silent[ch] = myIngest[ch].silent_run >= myCapacity ? 1 : 0;
	}
	myResetPending = false;
}

// Pipeline owner thread: worker (Async on) or cook thread (Async off)
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
void
FFT::startWorker()
{
	if (myWorkerRunning) return;
	myWorkerStop.store(false, std::memory_order_release);
	myWorkerDormant.store(false, std::memory_order_release);
	myWorkerRunning = true;
	myWorker = std::thread([this]() { workerLoop(); });
}

void
FFT::stopWorker()
{
	if (!myWorkerRunning) return;
	myWorkerStop.store(true, std::memory_order_release);
	myWake.signal();
	if (myWorker.joinable()) myWorker.join();
	myWorkerRunning = false;
}

// Hot: poll the job slot every kWorkerPollMs (the cook never pays for a kernel wake-up).
// Dormant (no job for kWorkerDormantAfterMs, e.g. TouchDesigner paused or the node not cooking):
// sleep until the cook signals once. The dormancy transition is a Dekker handshake with the
// cook (store dormant; full fence; re-check the job slot) so a job published at that instant
// is never left unprocessed.
void
FFT::workerLoop()
{
#ifdef _WIN32
	nameAndBoostCurrentThread(L"FFT Custom CHOP analysis");
#endif
	FFTDSP::DenormalGuard ftz;
	auto lastJob = clk::now();
	for (;;) {
		if (myWorkerStop.load(std::memory_order_acquire)) return;
		if (myJobs.acquire()) {                          // latest job wins; older unconsumed jobs were overwritten
			runJob(myJobs.front());
			lastJob = clk::now();
			myWorkerDormant.store(false, std::memory_order_relaxed);
			continue;
		}
		if (!myWorkerDormant.load(std::memory_order_relaxed)) {
			if (std::chrono::duration<double, std::milli>(clk::now() - lastJob).count() > kWorkerDormantAfterMs) {
				myWorkerDormant.store(true, std::memory_order_seq_cst);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				continue;                                // re-check the slot before sleeping (pairs with the cook's fence)
			}
			myWake.waitFor(kWorkerPollMs);
		} else {
			myWake.wait();
		}
	}
}

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
	myPeakMagnitude = res.peakMag;
	myPeakFrequencyHz = res.peakHz;
	myHoldFrames = (res.seq <= myJobSeq) ? static_cast<int>(myJobSeq - res.seq) : 0;
}

// ---------------------------------------------------------------------------------------------
// Cook
// ---------------------------------------------------------------------------------------------
void
FFT::executeImpl(CHOP_Output* output, const OP_Inputs* inputs)
{
	myExecuteCount++;
	if (!output || !output->channels || !inputs) return;
	if (!myCpuOk) { zeroOutputSafe(output); return; }

	const auto t_start = clk::now();
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
	double sr = 44100.0;
	if (cinput && cinput->sampleRate > 0) sr = cinput->sampleRate;
	mySampleRate = std::clamp(sr, Parameters::kMinSampleRate, Parameters::kMaxSampleRate);

	double dt_ms = 1000.0 / 60.0;
	if (const OP_TimeInfo* ti = inputs->getTimeInfo()) {
		if (ti->deltaMS > 0.0 && ti->deltaMS < 5000.0) dt_ms = ti->deltaMS;
		else if (ti->rate > 0.0) dt_ms = 1000.0 / ti->rate;
		if (ti->rate > 0.0) myCookRate.store(ti->rate, std::memory_order_relaxed);
	}
	myCookDtMs.store(dt_ms, std::memory_order_relaxed);    // outputBandwidth() reads it (Info callbacks)

	const int win_samples = windowSamplesFrom(p, mySampleRate);
	myCapacity = static_cast<size_t>(win_samples);

	// --- 2. ingest ---
	myExecStage = 2;
	const int num_channels = std::min(analysisChannelCount(cinput, p.chanMode), std::max(1, output->numChannels));
	myAnalysisChannels = num_channels;
	ingest(cinput, p.chanMode, num_channels, p, myCapacity);

	// --- 3. analysis job ---
	myExecStage = 3;
	const bool want_async = p.async;
	if (want_async != myWorkerRunning) {
		if (want_async) startWorker(); else stopWorker();
	}
	myAsyncActive = myWorkerRunning;

	// Every cook publishes a job. (v2.7.0 removed "Update Every N Cooks", which used to skip this on
	// N-1 cooks out of N: with the analysis already off the cook thread, skipping bought nothing and
	// only halved the rate at which the spectrum updated.)
	fillJob(myJobs.back(), num_channels, win_samples, dt_ms);
	if (myJobs.publish()) myJobsDropped.fetch_add(1, std::memory_order_relaxed);   // worker had not taken the previous job
	if (myAsyncActive) {
		// The hot worker polls the slot itself. Only a dormant worker needs a kernel wake-up
		// (~5 us): fence + load pair with the worker's store + fence, so exactly one side always
		// sees the other and the job is picked up either way.
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (myWorkerDormant.load(std::memory_order_seq_cst)) myWake.signal();
	} else if (myJobs.acquire()) {
		runJob(myJobs.front());                           // inline on the cook thread
	}

	// --- 4. output (hold the previous spectrum when nothing new has been published) ---
	myExecStage = 4;
	copyResultsToOutput(output, num_channels);

	// --- 5. deferred Textport log (lock-free check; only locks when something was logged) ---
	if (myLog.hasPending()) myLog.flushToTextport();
	myLastCookUs = usSince(t_start);
	myExecStage = 0;
}

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

void
FFT::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1)
{
	// By reference: this function runs once per row (~276 times per cook) and an earlier by-value version
	// copied two std::strings on each of those calls. See the block comment above statusSnapshot().
	const AnalysisPipeline::Status& s = statusSnapshot();
	const double dspUs = myDspUs.load(std::memory_order_relaxed);
	char tempBuffer[256];
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

void
FFT::getWarningString(OP_String* warning, void* reserved1)
{
	// hold_frames == 1 is the normal one-frame latency of the async pipeline
	if (myAsyncActive && myHoldFrames > 3) {
		warning->setString("Analysis worker is falling behind (holding the previous spectrum); reduce Zero-Pad Len or Output Bins.");
	}
}

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

void
FFT::setupParameters(OP_ParameterManager* manager, void* reserved1)
{
	Parameters::setup(manager);
}

void
FFT::pulsePressed(const char* name, void* reserved1)
{
	if (!strcmp(name, Parameters::ResetName))
	{
		for (auto& st : myIngest) st.eq.reset();
		myResetPending = true;
		myErrorText.clear();
	}
}
