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
 * Class Implementation: FFT
 *
 * DETAILED OPERATIONAL WORKFLOW & PERFORMANCE EXPLANATIONS:
 * ---------------------------------------------------------------------------
 * 1. Plugin Registration & Factory:
 *    TouchDesigner loads the compiled DLL dynamically. FillCHOPPluginInfo(),
 *    CreateCHOPInstance(), and DestroyCHOPInstance() serve as C-ABI entry points.
 *
 * 2. Multi-Channel Input Processing Loop (execute()):
 *    - Step A: Fetches current UI parameter states from TouchDesigner's OP_Inputs manager.
 *    - Step B: Checks parameter caches. If window size or FFT pad choice changed,
 *              triggers rebuildDSP(), which pre-computes instant FFTW plans
 *              via FFTW_ESTIMATE and zero-fills padded_frame once.
 *    - Step C: If psychoacoustic scale or window type changed, triggers updateWarpingAndWindow(),
 *              which builds unrolled frequency index lookup tables with identity bypass.
 *    - Step D: Loops through each audio channel:
 *              1. Ingests incoming CHOP audio slice into circular FIFOBuffer via fast memcpy.
 *              2. Extracts contiguous sample history frame from FIFO.
 *              3. Applies Direct Form II Transposed High/Low Shelving EQ (zero-copy bypass if inactive).
 *              4. Multiplies tapering window directly into active slice [pad_start, pad_start + win_len)
 *                 of persistent zero-padded frame using 2x unrolled AVX2 SIMD (_mm256_mul_ps). Zero-padded boundaries
 *                 remain zero persistently, eliminating millions of zero-writes per second.
 *              5. Executes R2C FFTW transform and evaluates magnitude spectrum via 2x unrolled 256-bit AVX2 SIMD.
 *              6. Re-maps linear magnitudes to psychoacoustic scale (with 1-to-1 identity bypass for linear grids).
 *              7. Applies Equal-Loudness Weighting Curve (2x unrolled AVX2 SIMD).
 *              8. Converts magnitudes to Decibels (dB) with parallel peak search and pre-computed db_offset.
 *              9. Applies Asymmetric Attack/Release Ballistics dynamic smoothing (2x unrolled AVX2 FMA, zero-copy when disabled).
 *              10. Copies output channels directly to TouchDesigner's CHOP_Output memory array via memcpy.
 * ===========================================================================
 */

#include "FFT.h"

#include <stdio.h>
#include <string.h>
#include <cmath>
#include <assert.h>
#include <algorithm>
#include <immintrin.h> // AVX2 (Advanced Vector Extensions 2) SIMD Compiler Intrinsics
#include <exception>

/// C-ABI Exported Functions for TouchDesigner Plugin Architecture
extern "C"
{

/**
 * @brief Fills CHOP Plugin Metadata (OP Type, Label, Author, Min/Max Inputs).
 *
 * TOUCHDESIGNER OPTYPE NAMING RULES & JUSTIFICATION:
 * - opType: Unique internal operator identifier registered with TouchDesigner.
 *   MUST start with an uppercase letter [A-Z], followed ONLY by lowercase letters [a-z] and digits [0-9].
 *   Must NOT conflict with TouchDesigner built-in operators (such as built-in 'fft' CHOP).
 *   Value: "Fftcustom" (Valid: 'F' + lowercase 'ftcustom').
 * - opLabel: Human-readable display label rendered in TouchDesigner dialogs and menus.
 *   Value: "FFT Custom".
 */
DLLEXPORT
void
FillCHOPPluginInfo(CHOP_PluginInfo *info)
{
	info->apiVersion = CHOPCPlusPlusAPIVersion;
	info->customOPInfo.opType->setString("Fftcustom");   // Internal registered opType
	info->customOPInfo.opLabel->setString("FFT Custom");  // TouchDesigner UI Display Label
	info->customOPInfo.opIcon->setString("FFT");         // 3-letter node icon in TouchDesigner networks
	info->customOPInfo.authorName->setString("Author");
	info->customOPInfo.authorEmail->setString("email@domain.com");
	info->customOPInfo.minInputs = 1; // Standalone or input-driven
	info->customOPInfo.maxInputs = 1;
}

/**
 * @brief Factory function called by TouchDesigner to instantiate the FFT class.
 */
DLLEXPORT
CHOP_CPlusPlusBase*
CreateCHOPInstance(const OP_NodeInfo* info)
{
	return new FFT(info);
}

/**
 * @brief Destructor entry point called by TouchDesigner upon operator deletion.
 */
DLLEXPORT
void
DestroyCHOPInstance(CHOP_CPlusPlusBase* instance)
{
	delete static_cast<FFT*>(instance);
}

} // extern "C"

namespace {

// Applies the tapering window into the active slice of the zero-padded frame (2x unrolled AVX2)
inline void applyWindow(const float* __restrict proc_data, const float* __restrict win_data, float* __restrict pad_data, size_t len) {
	size_t wi = 0;
#if defined(__AVX2__)
	for (; wi + 15 < len; wi += 16) {
		__m256 p0 = _mm256_load_ps(proc_data + wi);
		__m256 w0 = _mm256_load_ps(win_data + wi);
		_mm256_store_ps(pad_data + wi, _mm256_mul_ps(p0, w0));

		__m256 p1 = _mm256_load_ps(proc_data + wi + 8);
		__m256 w1 = _mm256_load_ps(win_data + wi + 8);
		_mm256_store_ps(pad_data + wi + 8, _mm256_mul_ps(p1, w1));
	}
	for (; wi + 7 < len; wi += 8) {
		__m256 p = _mm256_load_ps(proc_data + wi);
		__m256 w = _mm256_load_ps(win_data + wi);
		_mm256_store_ps(pad_data + wi, _mm256_mul_ps(p, w));
	}
#endif
	for (; wi < len; ++wi) {
		pad_data[wi] = proc_data[wi] * win_data[wi];
	}
}

// Multiplies the spectrum by the equal-loudness weighting curve in place (2x unrolled AVX2)
inline void applyWeightingCurve(float* __restrict spectrum, const float* __restrict curve, size_t len) {
	size_t wk = 0;
#if defined(__AVX2__)
	for (; wk + 15 < len; wk += 16) {
		__m256 s0 = _mm256_load_ps(spectrum + wk);
		__m256 c0 = _mm256_load_ps(curve + wk);
		_mm256_store_ps(spectrum + wk, _mm256_mul_ps(s0, c0));

		__m256 s1 = _mm256_load_ps(spectrum + wk + 8);
		__m256 c1 = _mm256_load_ps(curve + wk + 8);
		_mm256_store_ps(spectrum + wk + 8, _mm256_mul_ps(s1, c1));
	}
	for (; wk + 7 < len; wk += 8) {
		__m256 s = _mm256_load_ps(spectrum + wk);
		__m256 c = _mm256_load_ps(curve + wk);
		_mm256_store_ps(spectrum + wk, _mm256_mul_ps(s, c));
	}
#endif
	for (; wk < len; ++wk) {
		spectrum[wk] *= curve[wk];
	}
}

// Finds the peak magnitude and its index in a spectrum buffer using AVX2.
// Processes 8 elements at a time, skipping chunks where no lane exceeds
// the current max (checked via sign-bit movemask on the difference).
// Falls back to scalar when n < 9 or AVX2 is unavailable.
inline float findPeakWithIndex(const float* __restrict data, size_t n, size_t& peak_idx) noexcept {
	peak_idx = 0;
	if (n == 0) return 0.0f;

	float max_val = data[0];

#if defined(__AVX2__)
	if (n >= 9) {
		size_t i = 1;
		for (; i + 7 < n; i += 8) {
			__m256 v = _mm256_load_ps(data + i);
			__m256 v_diff = _mm256_sub_ps(v, _mm256_set1_ps(max_val));
			if (_mm256_movemask_ps(v_diff) != 0xFF) {
				alignas(32) float tmp[8];
				_mm256_store_ps(tmp, v);
				for (int j = 0; j < 8; ++j) {
					if (tmp[j] > max_val) {
						max_val = tmp[j];
						peak_idx = i + j;
					}
				}
			}
		}
		for (; i < n; ++i) {
			if (data[i] > max_val) {
				max_val = data[i];
				peak_idx = i;
			}
		}
	} else {
		for (size_t i = 1; i < n; ++i) {
			if (data[i] > max_val) {
				max_val = data[i];
				peak_idx = i;
			}
		}
	}
#else
	for (size_t i = 1; i < n; ++i) {
		if (data[i] > max_val) {
			max_val = data[i];
			peak_idx = i;
		}
	}
#endif

	return max_val;
}

} // anonymous namespace

/**
 * @brief Constructor: Sets up default internal state variables.
 */
FFT::FFT(const OP_NodeInfo* info)
	: myNodeInfo(info)
	, myExecuteCount(0)
	, mySampleRate(44100.0)
	, myBufferCapacity(3175)
	, myFFTSize(32768)
{
}

/**
 * @brief Destructor: Automatically releases FFTW plans via FFTWEngine destructor.
 */
FFT::~FFT()
{
}

/**
 * @brief Informs TouchDesigner that this node cooks on every frame (cookEveryFrame = true).
 */
void
FFT::getGeneralInfo(CHOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1)
{
	ginfo->cookEveryFrame = true;  // Guarantees continuous real-time spectrum updates
	ginfo->timeslice = false;       // Spectrum output is a full frequency frame, not audio timeslice
}

/**
 * @brief Configures output channel count, output sample count (bins), and output sample rate.
 */
bool
FFT::getOutputInfo(CHOP_OutputInfo* info, const OP_Inputs* inputs, void* reserved1)
{
	int bins = inputs->getParInt("Bins");

	if (bins <= 0) bins = 16384;

	if (inputs->getNumInputs() > 0)
	{
		const OP_CHOPInput* cinput = inputs->getInputCHOP(0);
		info->startIndex = 0;
		info->numChannels = cinput->numChannels;
		info->numSamples = bins;
		info->sampleRate = cinput->sampleRate;
	}
	else
	{
		info->startIndex = 0;
		info->numChannels = 1;
		info->numSamples = bins;
		info->sampleRate = bins;
	}
	return true;
}

/**
 * @brief Sets output channel names in TouchDesigner (e.g. chan1_fft, chan2_fft).
 */
void
FFT::getChannelName(int32_t index, OP_String *name, const OP_Inputs* inputs, void* reserved1)
{
	if (inputs->getNumInputs() > 0)
	{
		const OP_CHOPInput* cinput = inputs->getInputCHOP(0);
		if (index < cinput->numChannels)
		{
			std::string cname = cinput->getChannelName(index);
			cname += "_fft";
			name->setString(cname.c_str());
			return;
		}
	}
	name->setString("rfft");
}

/**
 * @brief Rebuilds DSP buffer capacities and FFTW plans when FFT size changes.
 *
 * PERFORMANCE EXPLANATION:
 * - FFTW_ESTIMATE: Instant, allocation-light plan creation (no benchmark sweep).
 * - Calling prepare() here ensures zero plan creation overhead inside execute().
 * - Zero-fills padded_frame ONCE during DSP rebuild so boundary zeroes stay persistent.
 */
void
FFT::rebuildDSPInternal(int engineChoice, double sr, int winSamples, int padChoice, int numBins)
{
	mySampleRate = sr;
	myBufferCapacity = std::max<size_t>(1, static_cast<size_t>(winSamples));

	size_t pad_size = static_cast<size_t>(padChoice);
	size_t needed = 1;
	while (needed < myBufferCapacity) {
		needed *= 2; // Find next power of two
	}
	myFFTSize = std::max<size_t>(pad_size, std::max<size_t>(needed, 2));

	// Instantiate selected CPU FFT Engine strategy
	switch (engineChoice) {
		case 1: // Intel MKL / IPP
			myFFTEngine = std::make_unique<FFTDSP::MKLEngine>();
			break;
		case 0: // FFTW3
		default:
			myFFTEngine = std::make_unique<FFTDSP::FFTWEngine>();
			break;
	}

	// Update cache FIRST so a failed prepare() doesn't trigger endless retries
	myCachedPadChoice = padChoice;
	myCachedEngine = engineChoice;
	myCachedScale = -1;

	if (myFFTEngine) {
		myFFTEngine->prepare(myFFTSize);
	}

	// Resize per-channel circular buffers, pre-zero padded_frame ONCE, and reset EQ states
	for (auto& ch : myChannels) {
		ch.initBuffers(myBufferCapacity, myFFTSize, static_cast<size_t>(numBins)); // Pre-zero padded_frame ONCE on rebuild
		ch.eq.setSampleRate(sr);
		ch.prev_spectrum.clear();
	}
}

/**
 * @brief Exception-safe wrapper around rebuildDSPInternal().
 */
void
FFT::rebuildDSP(int engineChoice, double sr, int winSamples, int padChoice, int numBins)
{
	try {
		rebuildDSPInternal(engineChoice, sr, winSamples, padChoice, numBins);
	} catch (const std::exception& e) {
		FFTDSP::logPlanEvent("[FFT Plugin] ERROR: Caught exception in rebuildDSP: " + std::string(e.what()) + " — retaining previous DSP state");
		if (!myFFTEngine) {
			myFFTEngine = std::make_unique<FFTDSP::FFTWEngine>();
			myFFTEngine->prepare(myFFTSize, FFTW_ESTIMATE); // Force safe flag
		}
	}
}

/**
 * @brief Updates psychoacoustic warping lookup tables, weighting curves, and window shapes.
 */
void
FFT::updateWarpingAndWindow(int scale, double displayMax, int bins, double warp, double logFloor, int winType, int kaiserBeta, int weighting)
{
	size_t n_linear_bins = myFFTSize / 2 + 1;
	double nyquist = mySampleRate / 2.0;
	double fmax = std::min(displayMax, nyquist);

	// Rebuild psychoacoustic re-mapping tables with pre-clamped indices and identity bypass detection
	myWarping.buildWarpTables(scale, fmax, static_cast<size_t>(bins), nyquist, warp, logFloor, n_linear_bins);

	// Pre-calculate equal-loudness weighting curve (A-Weighting / C-Weighting / ITU-R 468)
	FFTDSP::EqualLoudness::computeCurve(weighting, myWarping.targetHz(), myWeightingCurve);

	// Generate coherent-gain normalized audio window
	FFTDSP::WindowGenerator::generateWindow(winType, kaiserBeta, myBufferCapacity, myWindowBuffer);

	// Cache parameters
	myCachedScale = scale;
	myCachedDisplayMax = displayMax;
	myCachedBins = bins;
	myCachedWarp = warp;
	myCachedLogFloor = logFloor;
	myCachedWindowType = winType;
	myCachedKaiserBeta = kaiserBeta;
	myCachedWeighting = weighting;
}

/**
 * @brief Zeros the output CHOP within safe bounds after a caught exception, ensuring
 *        TouchDesigner receives silence instead of garbage or null pointer dereference.
 */
void
FFT::zeroOutputSafe(CHOP_Output* output) noexcept
{
	if (!output || !output->channels) return;
	int chans = std::max(0, std::min(output->numChannels, 64));
	int samples = std::max(0, output->numSamples);
	for (int c = 0; c < chans; ++c) {
		if (output->channels[c]) {
			std::memset(output->channels[c], 0, samples * sizeof(float));
		}
	}
}

/**
 * @brief Internal implementation of execute() — the full FFT analysis pipeline.
 *        The public execute() wrapper provides crash protection around this.
 *
 * HARDWARE & LATENCY OPTIMIZATION HIGHLIGHTS:
 * 1. Zero Allocation & Persistent Zero-Padding: All work vectors are pre-sized during rebuildDSP().
 *    Zero-filling of padded_frame is executed ONCE during rebuild, eliminating millions of zero-writes per second.
 * 2. EQ & Ballistics Bypass: Memory copies are completely skipped when EQ or envelope ballistics are disabled.
 * 3. 2x Unrolled AVX2 SIMD Windowing: Multiplies audio samples by window values 16 floats per cycle into active slice.
 * 4. FFT R2C Execution: Executes FFT plan with 0 plan creation overhead (FFTW_ESTIMATE).
 * 5. 2x Unrolled AVX2 Magnitude Calculation: Evaluates 8 complex bins per iteration using 256-bit SIMD registers.
 * 6. Identity Warp Bypass: Uses fast memcpy when spectrum mapping is 1-to-1 linear identity.
 * 7. Fast Direct Output Copy: Uses std::memcpy to write output channels to TouchDesigner memory.
 */
void
FFT::executeImpl(CHOP_Output* output, const OP_Inputs* inputs)
{
	myExecuteCount++;

	// Defensive: null-check and clamp host-provided values
	if (!output || !output->channels || !inputs) {
		return;
	}

	myExecStage = 1; // Parameter fetch
	// Fetch UI parameter values from TouchDesigner
	int engine_choice   = inputs->getParInt("Engine");
	int scale          = inputs->getParInt("Scale");
	double display_max = inputs->getParDouble("Displaymax");
	int bins           = inputs->getParInt("Bins");
	double warp        = inputs->getParDouble("Warp");
	double log_floor   = inputs->getParDouble("Logfloor");
	int win_samples    = inputs->getParInt("Winsamples");
	if (win_samples <= 0) win_samples = 3175;

	double gain_db     = inputs->getParDouble("Gaindb");
	double cutoff_hz   = inputs->getParDouble("Cutoffhz");
	double low_gain_db = inputs->getParDouble("Lowgaindb");
	double low_cutoff_hz = inputs->getParDouble("Lowcutoffhz");
	double q_factor    = inputs->getParDouble("Q");
	double amount      = inputs->getParDouble("Amount");

	int window_type    = inputs->getParInt("Window");
	int kaiser_beta    = inputs->getParInt("Kaiser");
	int weighting      = inputs->getParInt("Weighting");

	int loudness       = inputs->getParInt("Loudness");
	double db_range    = inputs->getParDouble("Dbrange");
	double attack      = inputs->getParDouble("Attack");
	double release     = inputs->getParDouble("Release");
	int pad_idx        = inputs->getParInt("Pad");

	int pad_choice = (pad_idx >= 0 && pad_idx < 7) ? Parameters::kPadValues[pad_idx] : 32768;
	if (bins <= 0) bins = 16384;
	if (display_max <= 0.0) display_max = 24000.0;

	// Cache input CHOP pointer once (avoids redundant API calls per frame)
	const OP_CHOPInput* cinput = (inputs->getNumInputs() > 0) ? inputs->getInputCHOP(0) : nullptr;

	// Ingest sample rate from input CHOP
	double sr = 44100.0;
	if (cinput && cinput->sampleRate > 0) {
		sr = cinput->sampleRate;
	}
	// Defensive clamp: sample rate to a sane range
	sr = std::max(1.0, std::min(192000.0, sr));

	myExecStage = 2; // DSP rebuild
	// Check if DSP engine rebuild is required
	bool rebuild_needed = false;
	if (std::abs(sr - mySampleRate) > 1e-3 || static_cast<size_t>(win_samples) != myBufferCapacity || pad_choice != myCachedPadChoice || engine_choice != myCachedEngine || !myFFTEngine) {
		rebuild_needed = true;
	}

	if (rebuild_needed) {
		rebuildDSP(engine_choice, sr, win_samples, pad_choice, bins);
	}

	myExecStage = 3; // Channel resize
	// Resize channel state array if channel count changed
	int num_channels = std::max(0, std::min(64, output->numChannels));
	if (static_cast<int>(myChannels.size()) != num_channels) {
		myChannels.resize(num_channels, ChannelState());
		for (auto& ch : myChannels) {
			ch.initBuffers(myBufferCapacity, myFFTSize, static_cast<size_t>(bins));
			ch.eq.setSampleRate(mySampleRate);
		}
	}

	myExecStage = 4; // Warping & windowing update
	// Recalculate warping lookup tables and window buffers if UI parameters changed
	if (myCachedScale != scale || std::abs(myCachedDisplayMax - display_max) > 1e-3 ||
		myCachedBins != bins || std::abs(myCachedWarp - warp) > 1e-5 ||
		std::abs(myCachedLogFloor - log_floor) > 1e-3 || myCachedWindowType != window_type ||
		myCachedKaiserBeta != kaiser_beta || myCachedWeighting != weighting || rebuild_needed) {

		updateWarpingAndWindow(scale, display_max, bins, warp, log_floor, window_type, kaiser_beta, weighting);
	}

	// Calculate zero-padding start offset to center windowed audio in FFT frame
	size_t pad_start = (myFFTSize > myBufferCapacity) ? ((myFFTSize - myBufferCapacity) / 2) : 0;

	myExecStage = 5; // Per-channel processing
	// Iterate through each audio channel independently
	for (int ch = 0; ch < num_channels; ++ch) {
		ChannelState& st = myChannels[ch];

		// 1. Ingest new audio samples from TouchDesigner input CHOP into FIFO ring buffer
		if (cinput) {
			int num_in_samples = cinput->numSamples;
			if (num_in_samples > 0) {
				const float* cdata = cinput->getChannelData(std::min(ch, cinput->numChannels - 1));
				st.fifo.add(cdata, static_cast<size_t>(num_in_samples));
			}
		}

		// 2. Extract linear sample sequence from circular FIFO
		st.fifo.get(st.captured_signal);

		// 3. Process audio frame through EQ (Zero-copy bypass if EQ is inactive)
		bool has_eq = st.eq.updateAndCheckActive(gain_db, cutoff_hz, low_gain_db, low_cutoff_hz, q_factor, amount);
		if (has_eq) {
			st.eq.processAudio(st.captured_signal, amount, st.processed_signal);
		}
		const float* proc_data = has_eq ? st.processed_signal.data() : st.captured_signal.data();

		const float* win_data = myWindowBuffer.data();
		float* pad_data = st.padded_frame.data() + pad_start;
		size_t win_len = std::min(myBufferCapacity, myWindowBuffer.size());

		// 5. 2x Unrolled AVX2 SIMD Windowing (16 floats per loop iteration)
		applyWindow(proc_data, win_data, pad_data, win_len);

		// 6. Execute selected Real-to-Complex FFT Transform (CPU or GPU)
		if (myFFTEngine) {
			myFFTEngine->executeRFFT(st.padded_frame, st.rfft_magnitude, st.scratch_complex);
		}

		// 7. Apply Psychoacoustic Frequency Scale Warping (Log/Mel/ERB/Bark/Chroma with Identity Bypass)
		myWarping.applyWarp(st.rfft_magnitude, st.warped_spectrum);

		// 8. Apply Equal-Loudness Weighting Curve (2x Unrolled AVX2 SIMD)
		if (weighting != 0 && myWeightingCurve.size() == st.warped_spectrum.size()) {
			applyWeightingCurve(st.warped_spectrum.data(), myWeightingCurve.data(), st.warped_spectrum.size());
		}

		// 9. Convert spectrum magnitudes to Decibels (dB) with parallel peak search & pre-computed db_offset
		if (loudness != 0) {
			if (st.prev_loudness_mode == 0) {
				st.prev_spectrum.clear();
			}
			FFTDSP::DecibelConverter::convertToDB(loudness, db_range, st.warped_spectrum);
		} else {
			if (st.prev_loudness_mode != 0) {
				st.prev_spectrum.clear();
			}
		}
		st.prev_loudness_mode = loudness;

		// 10. Apply Asymmetric Attack/Release Dynamic Ballistics Envelope Smoothing (2x Unrolled AVX2 FMA, Zero-copy when disabled)
		if (attack > 0.0 || release > 0.0) {
			st.ballistics.apply(static_cast<float>(attack), static_cast<float>(release), st.warped_spectrum, st.prev_spectrum);
		}

		// 11. Write output spectrum values directly to TouchDesigner CHOP output memory channels via memcpy
		size_t out_len = std::min(static_cast<size_t>(output->numSamples), st.warped_spectrum.size());
		float* out_chan = output->channels[ch];
		const float* spec_data = st.warped_spectrum.data();

		std::memcpy(out_chan, spec_data, out_len * sizeof(float));
		if (static_cast<size_t>(output->numSamples) > out_len) {
			std::memset(out_chan + out_len, 0, (output->numSamples - out_len) * sizeof(float));
		}

		// 12. Real-Time Telemetry: Track primary spectral peak frequency (Hz) and magnitude for Info CHOP/DAT
		if (ch == 0 && !st.warped_spectrum.empty()) {
			size_t max_idx = 0;
			myPeakMagnitude = findPeakWithIndex(st.warped_spectrum.data(), st.warped_spectrum.size(), max_idx);
			const auto& target_hz = myWarping.targetHz();
			if (max_idx < target_hz.size()) {
				myPeakFrequencyHz = static_cast<float>(target_hz[max_idx]);
			}
		}
	}

	myExecStage = 0; // Reset stage marker on successful completion
}

/**
 * @brief Exception-safe entry point for TouchDesigner. Wraps executeImpl() in a
 *        try/catch so a single bad frame zeros output instead of crashing.
 */
void
FFT::execute(CHOP_Output* output, const OP_Inputs* inputs, void* reserved)
{
	try {
		executeImpl(output, inputs);
	} catch (const std::exception& e) {
		FFTDSP::logPlanEvent("[FFT Plugin] ERROR: Caught exception in execute() at stage " + std::to_string(myExecStage) + ": " + std::string(e.what()) + " — zeroing output channels");
		zeroOutputSafe(output);
	} catch (...) {
		FFTDSP::logPlanEvent("[FFT Plugin] ERROR: Caught unknown exception in execute() at stage " + std::to_string(myExecStage) + " — zeroing output channels");
		zeroOutputSafe(output);
	}
}

/**
 * @brief Returns the number of diagnostic numeric channels exported to an Info CHOP.
 *
 * TOUCHDESIGNER INTEGRATION RATIONALE:
 * Info CHOPs allow operators to publish live numeric telemetry (cook counts, peak frequencies,
 * hardware acceleration status) to downstream TouchDesigner networks without affecting audio channels.
 */
int32_t
FFT::getNumInfoCHOPChans(void* reserved1)
{
	return 7;
}

void
FFT::getInfoCHOPChan(int index, OP_InfoCHOPChan* chan, void* reserved1)
{
	switch (index) {
	case 0:
		chan->name->setString("execute_count");
		chan->value = static_cast<float>(myExecuteCount);
		break;
	case 1:
		chan->name->setString("fft_size");
		chan->value = static_cast<float>(myFFTSize);
		break;
	case 2:
		chan->name->setString("window_samples");
		chan->value = static_cast<float>(myBufferCapacity);
		break;
	case 3:
		chan->name->setString("sample_rate");
		chan->value = static_cast<float>(mySampleRate);
		break;
	case 4:
		chan->name->setString("peak_freq_hz");
		chan->value = myPeakFrequencyHz;
		break;
	case 5:
		chan->name->setString("peak_magnitude");
		chan->value = myPeakMagnitude;
		break;
	case 6:
		chan->name->setString("simd_avx2_active");
#if defined(__AVX2__)
		chan->value = 1.0f;
#else
		chan->value = 0.0f;
#endif
		break;
	}
}

/**
 * @brief Configures diagnostic text table size exported to an Info DAT.
 */
bool
FFT::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	auto& logs = FFTDSP::getPlanLogHistory();
	infoSize->rows = 8 + static_cast<int32_t>(logs.size());
	infoSize->cols = 2;
	infoSize->byColumn = false;
	return true;
}

/**
 * @brief Populates text rows for an Info DAT attached to this Custom Operator.
 */
void
FFT::getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1)
{
	char tempBuffer[256];

	if (index < 8) {
		switch (index) {
		case 0:
			entries->values[0]->setString("execute_count");
			snprintf(tempBuffer, sizeof(tempBuffer), "%d", myExecuteCount);
			entries->values[1]->setString(tempBuffer);
			break;
		case 1:
			entries->values[0]->setString("fft_size");
			snprintf(tempBuffer, sizeof(tempBuffer), "%zu", myFFTSize);
			entries->values[1]->setString(tempBuffer);
			break;
		case 2:
			entries->values[0]->setString("window_samples");
			snprintf(tempBuffer, sizeof(tempBuffer), "%zu", myBufferCapacity);
			entries->values[1]->setString(tempBuffer);
			break;
		case 3:
			entries->values[0]->setString("sample_rate");
			snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", mySampleRate);
			entries->values[1]->setString(tempBuffer);
			break;
		case 4:
			entries->values[0]->setString("nyquist_frequency");
			snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", mySampleRate / 2.0);
			entries->values[1]->setString(tempBuffer);
			break;
		case 5:
			entries->values[0]->setString("spectral_peak_freq");
			snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", myPeakFrequencyHz);
			entries->values[1]->setString(tempBuffer);
			break;
		case 6:
			entries->values[0]->setString("simd_acceleration");
#if defined(__AVX2__)
			entries->values[1]->setString("AVX2 256-bit SIMD");
#else
			entries->values[1]->setString("Scalar Fallback");
#endif
			break;
		case 7:
			entries->values[0]->setString("fft_engine");
			if (myFFTEngine) {
				entries->values[1]->setString(myFFTEngine->getPlanStatus().c_str());
			} else {
				entries->values[1]->setString("Uninitialized");
			}
			break;
		}
	} else {
		auto& logs = FFTDSP::getPlanLogHistory();
		size_t log_idx = static_cast<size_t>(index - 8);
		if (log_idx < logs.size()) {
			snprintf(tempBuffer, sizeof(tempBuffer), "plan_log_%zu", log_idx);
			entries->values[0]->setString(tempBuffer);
			entries->values[1]->setString(logs[log_idx].c_str());
		}
	}
}

void
FFT::getInfoPopupString(OP_String *info, void *reserved1)
{
	std::string text = "TouchDesigner Custom FFT Plugin\n";
	text += "Active Engine & Plan: " + (myFFTEngine ? myFFTEngine->getPlanStatus() : "Uninitialized") + "\n";
	text += "FFT Size: N = " + std::to_string(myFFTSize) + " | Buffer Capacity: " + std::to_string(myBufferCapacity) + " samples\n";
	text += "Sample Rate: " + std::to_string(mySampleRate) + " Hz\n";
	text += "SIMD Acceleration: AVX2 256-Bit FMA Vectorized (2x Unrolled)\n\n";
	text += "--- Recent Plan Event Logs ---\n";
	auto& logs = FFTDSP::getPlanLogHistory();
	size_t start_idx = logs.size() > 5 ? logs.size() - 5 : 0;
	for (size_t i = start_idx; i < logs.size(); ++i) {
		text += logs[i] + "\n";
	}
	info->setString(text.c_str());
}

void
FFT::getWarningString(OP_String *warning, void *reserved1)
{
	if (myExecuteCount > 0 && myBufferCapacity > myFFTSize) {
		warning->setString("Window sampling size exceeds zero-padded FFT size; audio window will be clipped.");
	}
}

void
FFT::getErrorString(OP_String *error, void *reserved1)
{
	if (mySampleRate <= 0.0) {
		error->setString("Invalid or missing audio sample rate from input CHOP.");
	}
}

/**
 * @brief Registers custom UI parameters with TouchDesigner's parameter manager.
 *
 * ARCHITECTURAL DESIGN PATTERN:
 * Delegates parameter creation to the modular Parameters::setup system based on Derivative templates.
 */
void
FFT::setupParameters(OP_ParameterManager* manager, void *reserved1)
{
	Parameters::setup(manager);
}

/**
 * @brief Pulse parameter callback for reset pulse button.
 */
void 
FFT::pulsePressed(const char* name, void* reserved1)
{
	if (!strcmp(name, "Reset"))
	{
		for (auto& ch : myChannels) {
			ch.prev_spectrum.clear();
		}
	}
}
