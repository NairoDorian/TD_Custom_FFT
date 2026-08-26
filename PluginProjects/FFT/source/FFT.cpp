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
 * Source File: FFT.cpp
 *
 * Per cook (execute):
 *   1. Parameters::eval() — one snapshot of every parameter (clamped).
 *   2. rebuildDSP() when sample rate / window length / FFT size / planner change.
 *   3. updateWindow() / updateWarp() / updateWeighting() — each only when its own key changes
 *      (dragging Display Max no longer regenerates the window; changing the window type no
 *      longer regenerates the warp tables).
 *   4. processChannel() for every channel — serial, or std::execution::par when the channel
 *      count reaches "Parallel Min Channels". Channels share only read-only tables.
 *   5. Peak telemetry from channel 0.
 * ===========================================================================
 */

#include "FFT.h"

#include <algorithm>
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
const int   kMinorVersion = 2;

} // namespace

// ---------------------------------------------------------------------------------------------
// C-ABI entry points (C++ API 10)
// ---------------------------------------------------------------------------------------------
extern "C"
{

DLLEXPORT
void
FillCHOPPluginInfo(CHOP_PluginInfo* info)
{
	// API 10: setAPIVersion() returns false when this TouchDesigner cannot load this API version.
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

// ---------------------------------------------------------------------------------------------
FFT::FFT(const OP_NodeInfo* info)
	: myNodeInfo(info)
{
	myCpuOk = g_cpuHasAVX2;
	if (!myCpuOk) {
		myErrorText = "This CPU has no AVX2/FMA support; the FFT plugin is compiled for AVX2 and will output silence.";
		myLog.log("[FFT Plugin] ERROR: " + myErrorText);
	}
}

FFT::~FFT()
{
}

void
FFT::getGeneralInfo(CHOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1)
{
	// Cook whenever something downstream asks for the spectrum (input audio changes every frame
	// anyway). Nothing is lost while idle: the FIFO re-fills with the latest audio on the next cook.
	ginfo->cookEveryFrame = false;
	ginfo->cookEveryFrameIfAsked = true;
	ginfo->timeslice = false;
	ginfo->inputMatchIndex = 0;
}

bool
FFT::getOutputInfo(CHOP_OutputInfo* info, const OP_Inputs* inputs, void* reserved1)
{
	int bins = std::clamp(inputs->getParInt(Parameters::BinsName), Parameters::kMinBins, Parameters::kMaxBins);
	if (inputs->getParInt(Parameters::BinsName) <= 0) bins = 16384;

	info->startIndex = 0;
	info->numSamples = bins;
	if (inputs->getNumInputs() > 0)
	{
		const OP_CHOPInput* cinput = inputs->getInputCHOP(0);
		info->numChannels = std::clamp(cinput ? cinput->numChannels : 1, 0, Parameters::kMaxChannels);
		info->sampleRate = (cinput && cinput->sampleRate > 0.0) ? static_cast<float>(cinput->sampleRate) : static_cast<float>(bins);
	}
	else
	{
		info->numChannels = 1;
		info->sampleRate = static_cast<float>(bins);
	}
	return true;
}

void
FFT::getChannelName(int32_t index, OP_String* name, const OP_Inputs* inputs, void* reserved1)
{
	if (inputs->getNumInputs() > 0)
	{
		const OP_CHOPInput* cinput = inputs->getInputCHOP(0);
		if (cinput && index < cinput->numChannels)
		{
			std::string cname = cinput->getChannelName(index);
			cname += "_fft";
			name->setString(cname.c_str());
			return;
		}
	}
	name->setString("rfft");
}

// ---------------------------------------------------------------------------------------------
// DSP (re)initialisation
// ---------------------------------------------------------------------------------------------
void
FFT::rebuildDSPInternal(double sr, int winSamples, int padChoice, FFTDSP::PlannerPolicy planner, int numBins)
{
	mySampleRate = sr;
	myBufferCapacity = std::max<size_t>(1, static_cast<size_t>(winSamples));

	// FFT size: at least the requested zero-pad length, and always >= next power of two of the window,
	// so the window can never overflow the padded frame.
	size_t needed = 1;
	while (needed < myBufferCapacity) needed *= 2;
	myFFTSize = std::max<size_t>(static_cast<size_t>(padChoice), std::max<size_t>(needed, 2));

	// Centre the window in the zero-padded frame. The start offset is rounded down to a multiple of
	// 8 floats so the SIMD window store is 32-byte aligned; a shift of <8 samples inside the frame
	// only changes the phase of the spectrum, never the magnitude we output.
	size_t centre = (myFFTSize > myBufferCapacity) ? (myFFTSize - myBufferCapacity) / 2 : 0;
	myPadStart = centre & ~static_cast<size_t>(7);

	myCachedPadChoice = padChoice;
	myCachedPlanner = planner;
	myWarpKey = WarpKey{};        // nlin / nyquist changed -> warp tables must be rebuilt
	myWindowKey = WindowKey{};    // capacity changed -> window must be rebuilt

	if (!myFFTEngine) myFFTEngine = std::make_unique<FFTDSP::FFTWEngine>();
	myFFTEngine->prepare(myFFTSize, planner, &myLog);

	for (auto& ch : myChannels) {
		ch.initBuffers(myBufferCapacity, myFFTSize, static_cast<size_t>(numBins));
		ch.eq.setSampleRate(sr);
	}
}

void
FFT::rebuildDSP(double sr, int winSamples, int padChoice, FFTDSP::PlannerPolicy planner, int numBins)
{
	try {
		rebuildDSPInternal(sr, winSamples, padChoice, planner, numBins);
		myErrorText.clear();
	} catch (const std::exception& e) {
		myErrorText = std::string("rebuildDSP failed: ") + e.what();
		myLog.log("[FFT Plugin] ERROR: " + myErrorText + " — retaining previous DSP state");
		if (!myFFTEngine) {
			myFFTEngine = std::make_unique<FFTDSP::FFTWEngine>();
			myFFTEngine->prepare(myFFTSize, FFTDSP::PlannerPolicy::Fast, &myLog);
		}
	}
}

void
FFT::updateWindow(const Parameters::Values& p)
{
	WindowKey key{ static_cast<int>(p.window), p.kaiserBeta, myBufferCapacity, static_cast<int>(p.magNorm) };
	if (key == myWindowKey && myWindowBuffer.size() == myBufferCapacity) return;
	FFTDSP::WindowGenerator::generateWindow(static_cast<int>(p.window), p.kaiserBeta, myBufferCapacity, myWindowBuffer,
	                                        p.magNorm == Parameters::MagNorm::FullScale ? FFTDSP::WindowNorm::FullScale
	                                                                                    : FFTDSP::WindowNorm::CoherentGain);
	myWindowKey = key;
}

void
FFT::updateWarp(const Parameters::Values& p)
{
	size_t n_linear_bins = myFFTSize / 2 + 1;
	double nyquist = mySampleRate / 2.0;
	double fmax = std::min(p.displayMax, nyquist);
	WarpKey key{ static_cast<int>(p.scale), fmax, p.bins, p.warp, p.logFloor, n_linear_bins, nyquist };
	if (key == myWarpKey) return;
	myWarping.buildWarpTables(static_cast<int>(p.scale), fmax, static_cast<size_t>(p.bins), nyquist, p.warp, p.logFloor, n_linear_bins);
	myWarpKey = key;
	++myWarpVersion;
}

void
FFT::updateWeighting(const Parameters::Values& p)
{
	WeightKey key{ static_cast<int>(p.weighting), myWarpVersion };
	if (key == myWeightKey && myWeightingCurve.size() == myWarping.outputBins()) return;
	FFTDSP::EqualLoudness::computeCurve(static_cast<int>(p.weighting), myWarping.targetHz(), myWeightingCurve);
	myWeightKey = key;
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
// Per-channel pipeline (thread-safe: touches only its ChannelState and read-only tables)
// ---------------------------------------------------------------------------------------------
void
FFT::processChannel(ChannelState& st, const OP_CHOPInput* cinput, int ch, CHOP_Output* output,
                    const Parameters::Values& p, float attackCoef, float releaseCoef, float agcDecay) noexcept
{
	// 1. Ingest new audio samples into the FIFO. The EQ (when active) runs here, on the NEW samples only,
	//    with continuous IIR state — each sample is filtered once, in time order. (Re-filtering the whole
	//    3175-sample window every frame cost ~16 us/channel and restarted the filter from a stale state.)
	if (cinput && cinput->numChannels > 0 && cinput->numSamples > 0) {
		const float* cdata = cinput->getChannelData(std::min(ch, cinput->numChannels - 1));
		const size_t n = static_cast<size_t>(cinput->numSamples);
		if (cdata) {
			const bool has_eq = p.eqEnable && st.eq.updateAndCheckActive(p.gainDb, p.cutoffHz, p.lowGainDb, p.lowCutoffHz, p.q, p.amount);
			if (has_eq) {
				// only the samples that will still be inside the window need filtering
				const size_t keep = std::min(n, st.fifo.capacity());
				const float* tail = cdata + (n - keep);
				if (st.eq_block.size() < keep) st.eq_block.resize(keep);
				std::memcpy(st.eq_block.data(), tail, keep * sizeof(float));
				st.eq.processBlockInPlace(st.eq_block.data(), keep, p.amount);
				st.fifo.add(st.eq_block.data(), keep);
			} else {
				st.fifo.add(cdata, n);
			}
		}
	}

	// 2. Linearize
	st.fifo.get(st.captured_signal);
	const float* proc_data = st.captured_signal.data();

	// 4. Window into the aligned centre of the persistent zero-padded frame
	size_t win_len = std::min(myBufferCapacity, myWindowBuffer.size());
	if (st.padded_frame.size() >= myPadStart + win_len) {
		FFTDSP::multiplyInto(proc_data, myWindowBuffer.data(), st.padded_frame.data() + myPadStart, win_len);
	}

	// 5. R2C FFT + magnitude
	if (myFFTEngine) myFFTEngine->executeRFFT(st.padded_frame, st.rfft_magnitude, st.scratch_complex);

	// 5b. Full-scale normalization: DC and Nyquist have no mirror bin, so they are 2x too large
	if (p.magNorm == Parameters::MagNorm::FullScale && st.rfft_magnitude.size() >= 2) {
		st.rfft_magnitude.front() *= 0.5f;
		st.rfft_magnitude.back() *= 0.5f;
	}

	// 6. Psychoacoustic warp (identity memcpy when 1:1)
	myWarping.applyWarp(st.rfft_magnitude, st.warped_spectrum);

	// 7. Equal-loudness weighting
	if (p.weighting != Parameters::Weighting::Off && myWeightingCurve.size() == st.warped_spectrum.size()) {
		FFTDSP::multiplyInPlace(st.warped_spectrum.data(), myWeightingCurve.data(), st.warped_spectrum.size());
	}

	// 8. Decibel conversion with the selected 0 dB reference
	const int loudness = static_cast<int>(p.loudness);
	if (loudness != 0) {
		if (st.prev_loudness_mode == 0) st.prev_spectrum.clear();
		float peak = FFTDSP::peakMagnitude(st.warped_spectrum.data(), st.warped_spectrum.size());
		float ref = 1.0f;
		switch (p.dbRef) {
			case Parameters::DbRef::Dbfs:
				// full-scale sine == 1.0 (FullScale) or N_win/2 (CoherentGain)
				ref = (p.magNorm == Parameters::MagNorm::FullScale) ? 1.0f : static_cast<float>(myBufferCapacity) * 0.5f;
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
		FFTDSP::DecibelConverter::convertToDB(loudness, p.dbRange, 1.0f / ref, st.warped_spectrum);
	} else if (st.prev_loudness_mode != 0) {
		st.prev_spectrum.clear();
	}
	st.prev_loudness_mode = loudness;

	// 9. Ballistics (zero-copy when disabled)
	if (attackCoef > 0.0f || releaseCoef > 0.0f) {
		st.ballistics.apply(attackCoef, releaseCoef, st.warped_spectrum, st.prev_spectrum);
	}

	// 10. Output
	size_t out_len = std::min(static_cast<size_t>(output->numSamples), st.warped_spectrum.size());
	float* out_chan = output->channels[ch];
	if (!out_chan) return;
	std::memcpy(out_chan, st.warped_spectrum.data(), out_len * sizeof(float));
	if (static_cast<size_t>(output->numSamples) > out_len) {
		std::memset(out_chan + out_len, 0, (output->numSamples - out_len) * sizeof(float));
	}
}

// ---------------------------------------------------------------------------------------------
void
FFT::executeImpl(CHOP_Output* output, const OP_Inputs* inputs)
{
	myExecuteCount++;
	if (!output || !output->channels || !inputs) return;
	if (!myCpuOk) { zeroOutputSafe(output); return; }

	const auto t_start = std::chrono::steady_clock::now();

	myExecStage = 1; // parameters
	int param_reads = 0;
	const Parameters::Values p = Parameters::eval(inputs, &param_reads);
	myLastParamReads = param_reads;
	myLastParamUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_start).count();

	const OP_CHOPInput* cinput = (inputs->getNumInputs() > 0) ? inputs->getInputCHOP(0) : nullptr;
	double sr = 44100.0;
	if (cinput && cinput->sampleRate > 0) sr = cinput->sampleRate;
	sr = std::clamp(sr, Parameters::kMinSampleRate, Parameters::kMaxSampleRate);

	// frame delta for frame-rate independent ballistics / AGC
	double dt_ms = 1000.0 / 60.0;
	if (const OP_TimeInfo* ti = inputs->getTimeInfo()) {
		if (ti->deltaMS > 0.0 && ti->deltaMS < 5000.0) dt_ms = ti->deltaMS;
		else if (ti->rate > 0.0) dt_ms = 1000.0 / ti->rate;
	}

	int win_samples = p.winSamples;
	if (p.winMode == Parameters::WinMode::Milliseconds) {
		win_samples = std::clamp(static_cast<int>(std::lround(p.winMs * sr / 1000.0)), 1, Parameters::kMaxWinSamples);
	}
	const FFTDSP::PlannerPolicy planner = static_cast<FFTDSP::PlannerPolicy>(p.planner);

	myExecStage = 2; // DSP rebuild
	bool rebuild_needed = std::abs(sr - mySampleRate) > 1e-3
	                   || static_cast<size_t>(win_samples) != myBufferCapacity
	                   || p.padSize != myCachedPadChoice
	                   || planner != myCachedPlanner
	                   || !myFFTEngine;
	if (rebuild_needed) rebuildDSP(sr, win_samples, p.padSize, planner, p.bins);
	// Swap in a background-measured plan when one is ready (main thread, before any channel executes)
	if (myFFTEngine) myFFTEngine->pollBackgroundPlan();

	myExecStage = 3; // channels
	int num_channels = std::clamp(output->numChannels, 0, Parameters::kMaxChannels);
	if (static_cast<int>(myChannels.size()) != num_channels) {
		size_t old = myChannels.size();
		myChannels.resize(num_channels);
		for (size_t i = old; i < myChannels.size(); ++i) {           // only NEW channels are (re)initialised
			myChannels[i].initBuffers(myBufferCapacity, myFFTSize, static_cast<size_t>(p.bins));
			myChannels[i].eq.setSampleRate(mySampleRate);
		}
	}

	myExecStage = 4; // tables (each rebuilt only when its own inputs change)
	updateWindow(p);
	updateWarp(p);
	updateWeighting(p);

	// ballistics coefficients (0/0 = bypassed)
	float attackCoef = 0.0f, releaseCoef = 0.0f;
	if (!p.ballEnable) {
		// section disabled: nothing to do
	} else if (p.ballMode == Parameters::BallisticsMode::Milliseconds) {
		attackCoef  = FFTDSP::BallisticsFilter::coefFromMs(p.attackMs, dt_ms);
		releaseCoef = FFTDSP::BallisticsFilter::coefFromMs(p.releaseMs, dt_ms);
	} else {
		attackCoef  = static_cast<float>(p.attack);
		releaseCoef = static_cast<float>(p.release);
	}
	const float agcDecay = FFTDSP::BallisticsFilter::coefFromMs(1500.0, dt_ms);

	myExecStage = 5; // per-channel processing
	myParallelActive = p.parallel && num_channels >= p.parallelMin;
	if (myParallelActive) {
		std::vector<int> idx(static_cast<size_t>(num_channels));
		std::iota(idx.begin(), idx.end(), 0);
		std::for_each(std::execution::par, idx.begin(), idx.end(), [&](int ch) {
			processChannel(myChannels[ch], cinput, ch, output, p, attackCoef, releaseCoef, agcDecay);
		});
	} else {
		for (int ch = 0; ch < num_channels; ++ch) {
			processChannel(myChannels[ch], cinput, ch, output, p, attackCoef, releaseCoef, agcDecay);
		}
	}

	myExecStage = 6; // telemetry (channel 0)
	if (num_channels > 0 && !myChannels[0].warped_spectrum.empty()) {
		const auto& spec = myChannels[0].warped_spectrum;
		size_t max_idx = 0;
		myPeakMagnitude = FFTDSP::findPeakWithIndex(spec.data(), spec.size(), max_idx);
		const auto& target_hz = myWarping.targetHz();
		if (max_idx < target_hz.size()) myPeakFrequencyHz = static_cast<float>(target_hz[max_idx]);
	}

	myLastCookUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_start).count();
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

// ---------------------------------------------------------------------------------------------
// Info CHOP / DAT / popup / diagnostics
// ---------------------------------------------------------------------------------------------
int32_t
FFT::getNumInfoCHOPChans(void* reserved1)
{
	return 12;
}

void
FFT::getInfoCHOPChan(int index, OP_InfoCHOPChan* chan, void* reserved1)
{
	switch (index) {
	case 0: chan->name->setString("execute_count");    chan->value = static_cast<float>(myExecuteCount); break;
	case 1: chan->name->setString("fft_size");         chan->value = static_cast<float>(myFFTSize); break;
	case 2: chan->name->setString("window_samples");   chan->value = static_cast<float>(myBufferCapacity); break;
	case 3: chan->name->setString("sample_rate");      chan->value = static_cast<float>(mySampleRate); break;
	case 4: chan->name->setString("peak_freq_hz");     chan->value = myPeakFrequencyHz; break;
	case 5: chan->name->setString("peak_magnitude");   chan->value = myPeakMagnitude; break;
	case 6: chan->name->setString("simd_avx2_active");
#if defined(__AVX2__)
		chan->value = myCpuOk ? 1.0f : 0.0f;
#else
		chan->value = 0.0f;
#endif
		break;
	case 7: chan->name->setString("parallel_active");  chan->value = myParallelActive ? 1.0f : 0.0f; break;
	case 8: chan->name->setString("cook_time_us");     chan->value = static_cast<float>(myLastCookUs); break;
	case 9: chan->name->setString("linear_bins");      chan->value = static_cast<float>(myFFTSize / 2 + 1); break;
	case 10: chan->name->setString("param_fetch_us");  chan->value = static_cast<float>(myLastParamUs); break;
	case 11: chan->name->setString("param_reads");     chan->value = static_cast<float>(myLastParamReads); break;
	}
}

bool
FFT::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	infoSize->rows = 12 + static_cast<int32_t>(myLog.size());
	infoSize->cols = 2;
	infoSize->byColumn = false;
	return true;
}

void
FFT::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1)
{
	char tempBuffer[256];
	auto row = [&](const char* k, const std::string& v) {
		entries->values[0]->setString(k);
		entries->values[1]->setString(v.c_str());
	};
	switch (index) {
	case 0: row("execute_count", std::to_string(myExecuteCount)); return;
	case 1: row("fft_size", std::to_string(myFFTSize)); return;
	case 2: row("linear_bins", std::to_string(myFFTSize / 2 + 1)); return;
	case 3: row("window_samples", std::to_string(myBufferCapacity)); return;
	case 4: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", mySampleRate); row("sample_rate", tempBuffer); return;
	case 5: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", mySampleRate / 2.0); row("nyquist_frequency", tempBuffer); return;
	case 6: snprintf(tempBuffer, sizeof(tempBuffer), "%.2f Hz", myBufferCapacity > 0 ? mySampleRate / static_cast<double>(myBufferCapacity) : 0.0); row("window_resolution", tempBuffer); return;
	case 7: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", myPeakFrequencyHz); row("spectral_peak_freq", tempBuffer); return;
	case 8:
#if defined(__AVX2__)
		row("simd_acceleration", myCpuOk ? "AVX2 256-bit FMA" : "UNSUPPORTED CPU (no AVX2)");
#else
		row("simd_acceleration", "Scalar");
#endif
		return;
	case 9: row("fft_engine", myFFTEngine ? myFFTEngine->getPlanStatus() : std::string("Uninitialized")); return;
	case 10: snprintf(tempBuffer, sizeof(tempBuffer), "%.1f us (params %.1f us / %d reads)%s", myLastCookUs, myLastParamUs, myLastParamReads, myParallelActive ? " (parallel)" : ""); row("cook_time", tempBuffer); return;
	case 11: row("wisdom_file", FFTDSP::FFTWEngine::wisdomPath()); return;
	default: break;
	}
	size_t log_idx = static_cast<size_t>(index - 12);
	if (log_idx < myLog.size()) {
		snprintf(tempBuffer, sizeof(tempBuffer), "plan_log_%zu", log_idx);
		row(tempBuffer, myLog.entry(log_idx));
	}
}

void
FFT::getInfoPopupString(OP_String* info, void* reserved1)
{
	std::string text = "TouchDesigner Custom FFT Plugin v" + std::to_string(kMajorVersion) + "." + std::to_string(kMinorVersion) + "\n";
	text += "Engine & Plan: " + (myFFTEngine ? myFFTEngine->getPlanStatus() : std::string("Uninitialized")) + "\n";
	text += "FFT Size: N = " + std::to_string(myFFTSize) + " | Window: " + std::to_string(myBufferCapacity) + " samples\n";
	text += "Sample Rate: " + std::to_string(mySampleRate) + " Hz | Cook: " + std::to_string(myLastCookUs) + " us"
	      + (myParallelActive ? " (parallel channels)" : "") + "\n";
	text += std::string("SIMD: ") + (myCpuOk ? "AVX2 256-bit FMA" : "UNSUPPORTED CPU") + "\n\n--- Recent Plan Event Logs ---\n";
	auto logs = myLog.snapshot();
	size_t start_idx = logs.size() > 5 ? logs.size() - 5 : 0;
	for (size_t i = start_idx; i < logs.size(); ++i) text += logs[i] + "\n";
	info->setString(text.c_str());
}

void
FFT::getWarningString(OP_String* warning, void* reserved1)
{
	if (myExecuteCount > 0 && myFFTEngine && myFFTEngine->fftSize() != myFFTSize) {
		warning->setString("FFT plan size does not match the configured FFT size; output is silent until the plan is rebuilt.");
	}
}

void
FFT::getErrorString(OP_String* error, void* reserved1)
{
	if (!myErrorText.empty()) {
		error->setString(myErrorText.c_str());
	} else if (mySampleRate <= 0.0) {
		error->setString("Invalid or missing audio sample rate from input CHOP.");
	}
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
		for (auto& ch : myChannels) {
			ch.prev_spectrum.clear();
			ch.agc_peak = 0.0f;
			ch.eq.reset();
		}
		myErrorText.clear();
	}
}
