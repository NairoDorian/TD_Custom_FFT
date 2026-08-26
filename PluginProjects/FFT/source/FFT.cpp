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
 * Cook (every frame, cook thread):
 *   1. FTZ/DAZ guard, parameters (polled every N cooks), input sample rate, frame delta
 *   2. ingest: mono-mix / first / per-channel block -> optional EQ (new samples only) -> FIFO,
 *      digital-silence tracking
 *   3. every "Update Every N Cooks": snapshot the windows into an AnalysisJob and either
 *        - post it to the worker (Async on; latest job wins, never blocks), or
 *        - run the pipeline inline (Async off)
 *   4. copy the last finished spectrum to the output (hold when nothing new)
 *   5. telemetry + deferred Textport log flush
 * ===========================================================================
 */

#include "FFT.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <numeric>
#include <string>

namespace {

bool g_cpuHasAVX2 = true;

const char* kOpType    = "Fftcustom";   // must not collide with the built-in FFT CHOP
const char* kOpLabel   = "FFT Custom";
const char* kOpIcon    = "FFT";
const int   kMajorVersion = 2;
const int   kMinorVersion = 3;

using clk = std::chrono::steady_clock;
inline double usSince(clk::time_point t0) { return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); }

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
	s.fftSize = myFFTSize;
	s.capacity = myCapacity;
	s.linearBins = myFFTSize / 2 + 1;
	s.magnitudeBins = myMagnitudeBins;
	s.dspUs = myLastUs;
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

	// FFT size >= zero-pad length and >= next power of two of the window (window never overflows the frame)
	size_t needed = 1;
	while (needed < myCapacity) needed *= 2;
	myFFTSize = std::max<size_t>(static_cast<size_t>(myPadChoice), std::max<size_t>(needed, 2));

	// 8-float aligned centre offset: a shift of < 8 samples only changes phase, never magnitude
	size_t centre = (myFFTSize > myCapacity) ? (myFFTSize - myCapacity) / 2 : 0;
	myPadStart = centre & ~static_cast<size_t>(7);

	myWindowKey = WindowKey{};
	myWarpKey = WarpKey{};

	if (!myEngine) myEngine = std::make_unique<FFTDSP::FFTWEngine>();
	myEngine->prepare(myFFTSize, myPlanner, myLog);

	for (auto& ch : myChannels) {
		ch.padded_frame.assign(myFFTSize, 0.0f);
		ch.rfft_magnitude.assign(myFFTSize / 2 + 1, 0.0f);
		ch.scratch_complex.resize(myFFTSize / 2 + 1);
		ch.prev_spectrum.clear();
		ch.agc_peak = 0.0f;
	}
	++myTablesVersion;
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

void
AnalysisPipeline::updateWarp(const Parameters::Values& p)
{
	size_t n_linear_bins = myFFTSize / 2 + 1;
	double nyquist = mySampleRate / 2.0;
	double fmax = std::min(p.displayMax, nyquist);
	WarpKey key{ static_cast<int>(p.scale), fmax, p.bins, p.warp, p.logFloor, n_linear_bins, nyquist, static_cast<int>(p.warpInterp) };
	if (key == myWarpKey) return;
	myWarping.setInterpolation(static_cast<int>(p.warpInterp));
	myWarping.buildWarpTables(static_cast<int>(p.scale), fmax, static_cast<size_t>(p.bins), nyquist, p.warp, p.logFloor, n_linear_bins);
	myMagnitudeBins = std::min(n_linear_bins, myWarping.maxLinearIndex() + 1);
	myWarpKey = key;
	++myWarpVersion;
	++myTablesVersion;
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
	if (out.size() != bins) out.resize(bins);

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
		ball.apply(attackCoef, releaseCoef, out, st.prev_spectrum);
		std::memcpy(out.data(), st.prev_spectrum.data(), bins * sizeof(float));
	}
}

void
AnalysisPipeline::process(const AnalysisJob& job, std::vector<FFTDSP::AlignedVector>& out)
{
	const auto t0 = clk::now();
	const Parameters::Values& p = job.p;

	const bool rebuild_needed = !myEngine
	                         || std::abs(job.sampleRate - mySampleRate) > 1e-3
	                         || static_cast<size_t>(job.winSamples) != myCapacity
	                         || p.padSize != myPadChoice
	                         || static_cast<FFTDSP::PlannerPolicy>(p.planner) != myPlanner;
	if (rebuild_needed) rebuild(job);
	myEngine->pollBackgroundPlan();

	if (myChannels.size() != static_cast<size_t>(job.numChannels)) {
		size_t old = myChannels.size();
		myChannels.resize(static_cast<size_t>(job.numChannels));
		for (size_t i = old; i < myChannels.size(); ++i) {
			myChannels[i].padded_frame.assign(myFFTSize, 0.0f);
			myChannels[i].rfft_magnitude.assign(myFFTSize / 2 + 1, 0.0f);
			myChannels[i].scratch_complex.resize(myFFTSize / 2 + 1);
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

	if (out.size() != static_cast<size_t>(job.numChannels)) out.resize(static_cast<size_t>(job.numChannels));
	for (int ch = 0; ch < job.numChannels; ++ch) {
		const bool silent = ch < static_cast<int>(job.silent.size()) && job.silent[ch] != 0;
		runChannel(myChannels[ch], job.windows[ch], silent, p, attackCoef, releaseCoef, agcDecay, out[ch]);
	}
	myLastUs = usSince(t0);
}

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
	ginfo->cookEveryFrame = false;
	ginfo->cookEveryFrameIfAsked = true;
	ginfo->timeslice = false;
	ginfo->inputMatchIndex = 0;
}

int
FFT::analysisChannelCount(const OP_CHOPInput* cinput, Parameters::ChanMode mode) const
{
	if (!cinput || cinput->numChannels <= 0) return 1;
	if (mode == Parameters::ChanMode::AllChannels) return std::clamp(cinput->numChannels, 1, Parameters::kMaxChannels);
	return 1;
}

bool
FFT::getOutputInfo(CHOP_OutputInfo* info, const OP_Inputs* inputs, void* reserved1)
{
	const int bins = Parameters::readBins(inputs);
	const OP_CHOPInput* cinput = (inputs->getNumInputs() > 0) ? inputs->getInputCHOP(0) : nullptr;
	info->startIndex = 0;
	info->numSamples = bins;
	info->numChannels = analysisChannelCount(cinput, Parameters::readChanMode(inputs));
	info->sampleRate = (cinput && cinput->sampleRate > 0.0) ? static_cast<float>(cinput->sampleRate) : static_cast<float>(bins);
	return true;
}

void
FFT::getChannelName(int32_t index, OP_String* name, const OP_Inputs* inputs, void* reserved1)
{
	const OP_CHOPInput* cinput = (inputs->getNumInputs() > 0) ? inputs->getInputCHOP(0) : nullptr;
	const Parameters::ChanMode mode = Parameters::readChanMode(inputs);
	if (cinput && cinput->numChannels > 0) {
		if (mode == Parameters::ChanMode::MonoMix && cinput->numChannels > 1) {
			name->setString("mix_fft");
			return;
		}
		if (index < cinput->numChannels) {
			std::string cname = cinput->getChannelName(index);
			cname += "_fft";
			name->setString(cname.c_str());
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
// Worker thread
// ---------------------------------------------------------------------------------------------
void
FFT::startWorker()
{
	if (myWorkerRunning) return;
	myWorkerStop = false;
	myWorkerRunning = true;
	myWorker = std::thread([this]() { workerLoop(); });
}

void
FFT::stopWorker()
{
	if (!myWorkerRunning) return;
	{
		std::lock_guard<std::mutex> lock(myJobMutex);
		myWorkerStop = true;
	}
	myJobCv.notify_all();
	if (myWorker.joinable()) myWorker.join();
	myWorkerRunning = false;
	myMailboxFull = false;
}

void
FFT::workerLoop()
{
	FFTDSP::DenormalGuard ftz;
	AnalysisJob job;
	std::vector<FFTDSP::AlignedVector> scratch;   // reused: the pipeline writes here, then we swap into the back buffer
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(myJobMutex);
			myJobCv.wait(lock, [this] { return myWorkerStop || myMailboxFull; });
			if (myWorkerStop) return;
			std::swap(job, myMailbox);      // take the latest job; keeps the mailbox's buffers for reuse
			myMailboxFull = false;
		}
		try {
			myPipeline->process(job, scratch);
		} catch (...) {
			continue;                        // keep the previous result on any failure
		}
		{
			std::lock_guard<std::mutex> lock(myResultMutex);
			int back = 1 - myFront;
			std::swap(myResults[back], scratch);
			myFront = back;
			myPublishedSeq = job.seq;
			refreshTelemetryLocked();
		}
	}
}

void
FFT::refreshTelemetryLocked()
{
	myStatusCopy = myPipeline->status();
	if (myPipeline->tablesVersion() != myTablesVersionSeen) {
		myTargetHzCopy = myPipeline->targetHz();
		myTablesVersionSeen = myPipeline->tablesVersion();
	}
}

void
FFT::publishSync(AnalysisJob& job)
{
	std::vector<FFTDSP::AlignedVector>& back = myResults[1 - myFront];
	myPipeline->process(job, back);
	std::lock_guard<std::mutex> lock(myResultMutex);
	myFront = 1 - myFront;
	myPublishedSeq = job.seq;
	refreshTelemetryLocked();
}

void
FFT::copyResultsToOutput(CHOP_Output* output, int numChannels)
{
	std::lock_guard<std::mutex> lock(myResultMutex);
	const auto& res = myResults[myFront];
	const size_t out_samples = static_cast<size_t>(std::max(0, output->numSamples));
	for (int ch = 0; ch < numChannels && ch < output->numChannels; ++ch) {
		float* dst = output->channels[ch];
		if (!dst) continue;
		if (ch < static_cast<int>(res.size()) && !res[ch].empty()) {
			size_t n = std::min(out_samples, res[ch].size());
			std::memcpy(dst, res[ch].data(), n * sizeof(float));
			if (out_samples > n) std::memset(dst + n, 0, (out_samples - n) * sizeof(float));
		} else {
			std::memset(dst, 0, out_samples * sizeof(float));
		}
	}
	// peak telemetry from channel 0 of the published result
	if (!res.empty() && !res[0].empty()) {
		size_t max_idx = 0;
		myPeakMagnitude = FFTDSP::findPeakWithIndex(res[0].data(), res[0].size(), max_idx);
		if (max_idx < myTargetHzCopy.size()) myPeakFrequencyHz = static_cast<float>(myTargetHzCopy[max_idx]);
	}
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

	// --- 1. parameters (polled) ---
	myExecStage = 1;
	const int poll = Parameters::readParamPoll(inputs);
	if (!myHaveParams || poll <= 1 || (myExecuteCount % poll) == 0) {
		myParams = Parameters::eval(inputs, &myParamReads);
		myParamReads += 1;   // + the poll read itself
		myHaveParams = true;
	} else {
		myParamReads = 1;
	}
	const Parameters::Values& p = myParams;
	myParamUs = usSince(t_start);

	const OP_CHOPInput* cinput = (inputs->getNumInputs() > 0) ? inputs->getInputCHOP(0) : nullptr;
	double sr = 44100.0;
	if (cinput && cinput->sampleRate > 0) sr = cinput->sampleRate;
	mySampleRate = std::clamp(sr, Parameters::kMinSampleRate, Parameters::kMaxSampleRate);

	double dt_ms = 1000.0 / 60.0;
	if (const OP_TimeInfo* ti = inputs->getTimeInfo()) {
		if (ti->deltaMS > 0.0 && ti->deltaMS < 5000.0) dt_ms = ti->deltaMS;
		else if (ti->rate > 0.0) dt_ms = 1000.0 / ti->rate;
	}

	int win_samples = p.winSamples;
	if (p.winMode == Parameters::WinMode::Milliseconds) {
		win_samples = std::clamp(static_cast<int>(std::lround(p.winMs * mySampleRate / 1000.0)), 1, Parameters::kMaxWinSamples);
	}
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

	const bool due = (p.updateEvery <= 1) || (myExecuteCount % p.updateEvery) == 0;
	if (due) {
		if (myAsyncActive) {
			std::lock_guard<std::mutex> lock(myJobMutex);
			if (myMailboxFull) myJobsDropped.fetch_add(1);   // worker still busy: latest job wins
			AnalysisJob& job = myMailbox;
			job.seq = ++myJobSeq;
			job.numChannels = num_channels;
			job.sampleRate = mySampleRate;
			job.winSamples = win_samples;
			job.dtMs = dt_ms;
			job.p = p;
			job.reset = myResetPending;
			job.windows.resize(static_cast<size_t>(num_channels));
			job.silent.resize(static_cast<size_t>(num_channels));
			for (int ch = 0; ch < num_channels; ++ch) {
				myIngest[ch].fifo.get(job.windows[ch]);
				job.silent[ch] = myIngest[ch].silent_run >= myCapacity ? 1 : 0;
			}
			myMailboxFull = true;
			myResetPending = false;
			myJobCv.notify_one();
		} else {
			AnalysisJob& job = myMailbox;     // reuse the buffers, no allocation after the first cook
			job.seq = ++myJobSeq;
			job.numChannels = num_channels;
			job.sampleRate = mySampleRate;
			job.winSamples = win_samples;
			job.dtMs = dt_ms;
			job.p = p;
			job.reset = myResetPending;
			job.windows.resize(static_cast<size_t>(num_channels));
			job.silent.resize(static_cast<size_t>(num_channels));
			for (int ch = 0; ch < num_channels; ++ch) {
				myIngest[ch].fifo.get(job.windows[ch]);
				job.silent[ch] = myIngest[ch].silent_run >= myCapacity ? 1 : 0;
			}
			myResetPending = false;
			publishSync(job);
		}
	}

	// --- 4. output (hold the previous spectrum when nothing new has been published) ---
	myExecStage = 4;
	copyResultsToOutput(output, num_channels);
	myHoldFrames = static_cast<int>(myJobSeq - myPublishedSeq);

	// --- 5. deferred Textport log ---
	myLog.flushToTextport();
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
// Info CHOP / DAT / popup / diagnostics
// =============================================================================================
int32_t
FFT::getNumInfoCHOPChans(void* reserved1)
{
	return 16;
}

void
FFT::getInfoCHOPChan(int index, OP_InfoCHOPChan* chan, void* reserved1)
{
	AnalysisPipeline::Status s;
	{
		std::lock_guard<std::mutex> lock(myResultMutex);
		s = myStatusCopy;
	}
	switch (index) {
	case 0:  chan->name->setString("execute_count");     chan->value = static_cast<float>(myExecuteCount); break;
	case 1:  chan->name->setString("fft_size");          chan->value = static_cast<float>(s.fftSize); break;
	case 2:  chan->name->setString("window_samples");    chan->value = static_cast<float>(s.capacity); break;
	case 3:  chan->name->setString("sample_rate");       chan->value = static_cast<float>(mySampleRate); break;
	case 4:  chan->name->setString("peak_freq_hz");      chan->value = myPeakFrequencyHz; break;
	case 5:  chan->name->setString("peak_magnitude");    chan->value = myPeakMagnitude; break;
	case 6:  chan->name->setString("simd_avx2_active");  chan->value = myCpuOk ? 1.0f : 0.0f; break;
	case 7:  chan->name->setString("async_active");      chan->value = myAsyncActive ? 1.0f : 0.0f; break;
	case 8:  chan->name->setString("cook_time_us");      chan->value = static_cast<float>(myLastCookUs); break;
	case 9:  chan->name->setString("dsp_time_us");       chan->value = static_cast<float>(s.dspUs); break;
	case 10: chan->name->setString("linear_bins");       chan->value = static_cast<float>(s.linearBins); break;
	case 11: chan->name->setString("param_fetch_us");    chan->value = static_cast<float>(myParamUs); break;
	case 12: chan->name->setString("param_reads");       chan->value = static_cast<float>(myParamReads); break;
	case 13: chan->name->setString("jobs_dropped");      chan->value = static_cast<float>(myJobsDropped.load()); break;
	case 14: chan->name->setString("analysis_channels"); chan->value = static_cast<float>(myAnalysisChannels); break;
	case 15: chan->name->setString("hold_frames");       chan->value = static_cast<float>(myHoldFrames); break;
	}
}

bool
FFT::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	infoSize->rows = 14 + static_cast<int32_t>(myLog.size());
	infoSize->cols = 2;
	infoSize->byColumn = false;
	return true;
}

void
FFT::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1)
{
	AnalysisPipeline::Status s;
	{
		std::lock_guard<std::mutex> lock(myResultMutex);
		s = myStatusCopy;
	}
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
	case 5: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", mySampleRate); row("sample_rate", tempBuffer); return;
	case 6: snprintf(tempBuffer, sizeof(tempBuffer), "%.2f Hz", s.capacity > 0 ? mySampleRate / static_cast<double>(s.capacity) : 0.0); row("window_resolution", tempBuffer); return;
	case 7: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", myPeakFrequencyHz); row("spectral_peak_freq", tempBuffer); return;
	case 8: row("simd_acceleration", myCpuOk ? "AVX2 256-bit FMA" : "UNSUPPORTED CPU (no AVX2)"); return;
	case 9: row("fft_engine", s.plan + (s.planUpgrading ? " [measuring better plan in background]" : "")); return;
	case 10: snprintf(tempBuffer, sizeof(tempBuffer), "cook %.1f us (params %.1f us / %d reads)", myLastCookUs, myParamUs, myParamReads); row("cook_time", tempBuffer); return;
	case 11: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f us per analysis (%s)", s.dspUs, myAsyncActive ? "off the cook thread" : "on the cook thread"); row("dsp_time", tempBuffer); return;
	case 12: snprintf(tempBuffer, sizeof(tempBuffer), "%llu dropped, hold %d frame(s)", static_cast<unsigned long long>(myJobsDropped.load()), myHoldFrames); row("async_jobs", tempBuffer); return;
	case 13: row("wisdom_file", FFTDSP::FFTWEngine::wisdomPath()); return;
	default: break;
	}
	size_t log_idx = static_cast<size_t>(index - 14);
	if (log_idx < myLog.size()) {
		snprintf(tempBuffer, sizeof(tempBuffer), "plan_log_%zu", log_idx);
		row(tempBuffer, myLog.entry(log_idx));
	}
}

void
FFT::getInfoPopupString(OP_String* info, void* reserved1)
{
	AnalysisPipeline::Status s;
	{
		std::lock_guard<std::mutex> lock(myResultMutex);
		s = myStatusCopy;
	}
	std::string text = "TouchDesigner Custom FFT Plugin v" + std::to_string(kMajorVersion) + "." + std::to_string(kMinorVersion) + "\n";
	text += std::string("Mode: ") + (myAsyncActive ? "async worker" : "sync") + " | " + std::to_string(myAnalysisChannels) + " analysis channel(s)\n";
	text += "Engine & Plan: " + s.plan + "\n";
	text += "FFT Size: N = " + std::to_string(s.fftSize) + " | Window: " + std::to_string(s.capacity) + " samples | magnitude bins computed: " + std::to_string(s.magnitudeBins) + "\n";
	text += "Cook: " + std::to_string(myLastCookUs) + " us (params " + std::to_string(myParamUs) + " us) | DSP: " + std::to_string(s.dspUs) + " us\n";
	text += std::string("SIMD: ") + (myCpuOk ? "AVX2 256-bit FMA" : "UNSUPPORTED CPU") + "\n\n--- Recent Plan Event Logs ---\n";
	auto logs = myLog.snapshot();
	size_t start_idx = logs.size() > 5 ? logs.size() - 5 : 0;
	for (size_t i = start_idx; i < logs.size(); ++i) text += logs[i] + "\n";
	info->setString(text.c_str());
}

void
FFT::getWarningString(OP_String* warning, void* reserved1)
{
	if (myAsyncActive && myHoldFrames > 3) {
		warning->setString("Analysis worker is falling behind (holding the previous spectrum); reduce Zero-Pad Len or Output Bins.");
	}
}

void
FFT::getErrorString(OP_String* error, void* reserved1)
{
	if (!myErrorText.empty()) error->setString(myErrorText.c_str());
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
