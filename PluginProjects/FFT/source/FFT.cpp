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
		// failed so it surfaces instead of vanishing silently into the textport.
		myPipelineErrors.fetch_add(1, std::memory_order_relaxed);
		myLog.log("[FFT Plugin] [pipeline] analysis threw an exception; previous spectrum retained");
		return;
	}
	myPlanFailed.store(myPipeline->planFailed(), std::memory_order_relaxed);
	res.seq = job.seq;
	myResults.publish();
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
AnalysisPipeline::Status
FFT::statusSnapshot()
{
	std::lock_guard<std::mutex> lock(myStatusMutex);
	return myStatusCopy;
}

int32_t
FFT::getNumInfoCHOPChans(void* reserved1)
{
	return 21;
}

void
FFT::getInfoCHOPChan(int index, OP_InfoCHOPChan* chan, void* reserved1)
{
	const AnalysisPipeline::Status s = statusSnapshot();
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
	infoSize->rows = 19 + static_cast<int32_t>(myLog.size());
	infoSize->cols = 2;
	infoSize->byColumn = false;
	return true;
}

void
FFT::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1)
{
	const AnalysisPipeline::Status s = statusSnapshot();
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
	default: break;
	}
	size_t log_idx = static_cast<size_t>(index - 19);
	if (log_idx < myLog.size()) {
		snprintf(tempBuffer, sizeof(tempBuffer), "plan_log_%zu", log_idx);
		row(tempBuffer, myLog.entry(log_idx));
	}
}

void
FFT::getInfoPopupString(OP_String* info, void* reserved1)
{
	const AnalysisPipeline::Status s = statusSnapshot();
	char buf[256];
	// Node identity first. Two things make this worth the three lines: the middle-click popup is the
	// only place the node can name itself and its own binary, and "which FFT.dll answered" is a real
	// question here - the plugin can be installed under Documents/Derivative/Plugins and also staged in
	// the project's __Plugins__, and the two copies are indistinguishable in a textport log that does
	// not say which process loaded which.
	const char* nodePath = (myNodeInfo && myNodeInfo->opPath) ? myNodeInfo->opPath : "<unknown>";
	const char* dllPath  = (myNodeInfo && myNodeInfo->pluginPath) ? myNodeInfo->pluginPath : "<unknown>";
	// cookCount is the node's own proof that it is cooking. Everything below it arrives through the
	// info callbacks, which TouchDesigner only calls inside a cook - so if this number is not climbing,
	// nothing else on this popup is live either. (That failure mode is why getGeneralInfo returns
	// cookEveryFrame = true; see the comment there.)
	const uint32_t cooks = myNodeInfo ? myNodeInfo->cookCount : 0u;
	std::string text = std::string("Node: ") + nodePath + " (" + kOpType + ", CHOP, " + std::to_string(myAnalysisChannels) + " channel(s))\n";
	text += "Plugin: TouchDesigner Custom FFT Plugin v" + std::to_string(kMajorVersion) + "." + std::to_string(kMinorVersion) + "\n";
	text += std::string("Binary: ") + dllPath + "\n";
	text += std::string("Mode: ") + (myAsyncActive ? "async worker" : "sync") + " | " + std::to_string(myAnalysisChannels) + " analysis channel(s)\n";
	text += "Engine & Plan: " + s.plan + "\n";
	text += "FFT Size: N = " + std::to_string(s.fftSize) + " | Window: " + std::to_string(s.capacity) + " samples | magnitude bins computed: " + std::to_string(s.magnitudeBins) + "\n";
	{
		const double axis_rate = outputAxisRate(myParams, mySampleRate);
		const int n_out = outputBinCountFrom(myParams);
		snprintf(buf, sizeof(buf), "Spectrum axis: %d bins @ %.2f Hz = %.1f..%.1f Hz (input %.1f Hz%s)\n",
		         n_out, hzPerSample(myParams, mySampleRate), s.axisBottom, axis_rate * 0.5, mySampleRate,
		         s.linearGrid ? ", linear grid: identity warp, no resampling" : ", resampled onto the warp grid");
		text += buf;
	}
	{
		// The reported sample rate is bins x me.time.rate; the measured throughput is the same idea
		// with the cook delta actually observed. Both are shown, plus the axis rate, because only the
		// axis rate converts a bin index to Hz.
		const double dt_ms = myCookDtMs.load(std::memory_order_relaxed);
		const double rate = myCookRate.load(std::memory_order_relaxed);
		const int n_out = outputBinCountFrom(myParams);
		snprintf(buf, sizeof(buf), "Sample rate (to TouchDesigner): %.0f Hz = %d bins x %.2f frames/s (me.time.rate)\n",
		         outputSampleRate(myParams), n_out, rate > 0.0 ? rate : 60.0);
		text += buf;
		snprintf(buf, sizeof(buf), "Measured throughput: %.0f samples/s (cook delta %.2f ms)\n",
		         outputBandwidth(myParams),
		         dt_ms);
		text += buf;
	}
	// The shape of what leaves the node, stated the way TouchDesigner sees it (samples per channel,
	// channel count, sample rate) - the same three numbers getOutputInfo() sets, so the popup and the
	// node's output can be compared without opening a CHOP viewer.
	snprintf(buf, sizeof(buf), "Output: %d samples/channel x %d channel(s) @ %.0f Hz | window %zu samples of input\n",
	         outputBinCountFrom(myParams), myAnalysisChannels, outputSampleRate(myParams),
	         static_cast<size_t>(s.capacity));
	text += buf;
	text += "Cook: " + std::to_string(myLastCookUs) + " us CPU (params " + std::to_string(myParamUs) + " us) | DSP: " + std::to_string(myDspUs.load(std::memory_order_relaxed)) + " us"
	     + " | cooks: " + std::to_string(cooks) + "\n"
	     // GPU cook time is deliberately not invented here: this node has no GPU stage at all, so
	     // there is nothing for it to report. TouchDesigner's own Operator Info header carries the
	     // node's CPU and GPU cook times for the frame; this line is the plugin's own measurement.
	     + "GPU: none (all stages are CPU/AVX2; TD's Operator Info header reports the per-node GPU time)\n";
	text += std::string("SIMD: ") + (myCpuOk ? "AVX2 256-bit FMA" : "UNSUPPORTED CPU") + "\n\n--- Recent Plan Event Logs ---\n";
	auto logs = myLog.snapshot();
	size_t start_idx = logs.size() > 5 ? logs.size() - 5 : 0;
	for (size_t i = start_idx; i < logs.size(); ++i) text += logs[i] + "\n";
	info->setString(text.c_str());
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
	else if (myPipelineErrors.load(std::memory_order_relaxed) > 0) error->setString((std::to_string(myPipelineErrors.load(std::memory_order_relaxed)) + " analysis pipeline error(s); see textport log.").c_str());
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
