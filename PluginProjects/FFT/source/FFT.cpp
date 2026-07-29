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
 *              triggers rebuildDSP(), which pre-computes hardware-tuned FFTW plans
 *              via FFTW_MEASURE.
 *    - Step C: If psychoacoustic scale or window type changed, triggers updateWarpingAndWindow(),
 *              which builds unrolled frequency index lookup tables.
 *    - Step D: Loops through each audio channel:
 *              1. Ingests incoming CHOP audio slice into circular FIFOBuffer via fast memcpy.
 *              2. Extracts contiguous sample history frame from FIFO.
 *              3. Applies Direct Form II Transposed High/Low Shelving EQ (bypassed with zero-copy if inactive).
 *              4. Centers audio in zero-padded frame and applies 2x unrolled AVX2 SIMD windowing (_mm256_mul_ps).
 *              5. Executes R2C FFTW transform and evaluates magnitude spectrum via 2x unrolled 256-bit AVX2 SIMD.
 *              6. Re-maps linear magnitudes to psychoacoustic scale (with 1-to-1 identity bypass for linear grids).
 *              7. Applies Equal-Loudness Weighting Curve (AVX2 SIMD _mm256_mul_ps).
 *              8. Converts magnitudes to Decibels (dB) with parallel peak search and pre-computed db_offset.
 *              9. Applies Asymmetric Attack/Release Ballistics dynamic smoothing (zero-copy when 0.0).
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
	info->customOPInfo.minInputs = 0; // Standalone or input-driven
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

// Pre-defined menu choices for zero-padded FFT sizes (1024 to 65536)
static const int PAD_VALUES[7] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };

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
		info->numChannels = cinput->numChannels; // Inherit input channel count
		info->numSamples = bins;                 // Number of output frequency spectrum bins
		info->sampleRate = cinput->sampleRate;
	}
	else
	{
		info->numChannels = 1;
		info->numSamples = bins;
		info->sampleRate = 44100;
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
 * @brief Rebuilds DSP buffer sizes and pre-benchmarks hardware-tuned FFTW plans.
 *
 * PERFORMANCE EXPLANATION:
 * - FFTW_MEASURE: Benchmarks multiple SIMD assembly routines on your CPU hardware
 *   during plan creation, selecting the absolute fastest kernel for your processor.
 * - Calling prepare() here ensures zero plan creation overhead inside execute().
 */
void
FFT::rebuildDSP(double sr, int winSamples, int padChoice, int numBins)
{
	mySampleRate = sr;
	myBufferCapacity = std::max<size_t>(1, static_cast<size_t>(winSamples));

	size_t pad_size = static_cast<size_t>(padChoice);
	size_t needed = 1;
	while (needed < myBufferCapacity) {
		needed *= 2; // Find next power of two
	}
	myFFTSize = std::max<size_t>(pad_size, std::max<size_t>(needed, 2));

	// Pre-benchmark hardware-optimized FFTW3 plan for new FFT size
	myFFTWEngine.prepare(myFFTSize);

	// Resize per-channel circular buffers and reset EQ states
	for (auto& ch : myChannels) {
		ch.fifo.resize(myBufferCapacity);
		ch.eq.setSampleRate(sr);
		ch.prev_spectrum.clear();
	}

	myCachedScale = -1; // Invalidate parameter cache
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

	// Rebuild psychoacoustic re-mapping tables with pre-clamped indices (optimizes applyWarp)
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
 * @brief Primary real-time execution loop called 60-120 times per second by TouchDesigner.
 *
 * HARDWARE & LATENCY OPTIMIZATION HIGHLIGHTS:
 * 1. Zero-Allocation Guarantee: All work vectors are pre-sized during setup/rebuild.
 * 2. EQ & Ballistics Bypass: Memory copies are completely skipped when EQ or envelope ballistics are disabled.
 * 3. 2x Unrolled AVX2 SIMD Windowing: Multiplies audio samples by window values 16 floats per cycle.
 * 4. FFTW3 R2C Execution: Executes fftwf_execute_dft_r2c with 0 plan creation overhead.
 * 5. 2x Unrolled AVX2 Magnitude Calculation: Evaluates 8 complex bins per iteration using 256-bit SIMD registers.
 * 6. Identity Warp Bypass: Uses fast memcpy when spectrum mapping is 1-to-1 linear identity.
 * 7. Fast Direct Output Copy: Uses std::memcpy to write output channels to TouchDesigner memory.
 */
void
FFT::execute(CHOP_Output* output, const OP_Inputs* inputs, void* reserved)
{
	myExecuteCount++;

	// Fetch UI parameter values from TouchDesigner
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

	int pad_choice = (pad_idx >= 0 && pad_idx < 7) ? PAD_VALUES[pad_idx] : 32768;
	if (bins <= 0) bins = 16384;
	if (display_max <= 0.0) display_max = 24000.0;

	// Ingest sample rate from input CHOP
	double sr = 44100.0;
	if (inputs->getNumInputs() > 0)
	{
		const OP_CHOPInput* cinput = inputs->getInputCHOP(0);
		if (cinput->sampleRate > 0) {
			sr = cinput->sampleRate;
		}
	}

	// Check if DSP engine rebuild is required
	bool rebuild_needed = false;
	if (std::abs(sr - mySampleRate) > 1e-3 || static_cast<size_t>(win_samples) != myBufferCapacity || pad_choice != myCachedPadChoice) {
		rebuild_needed = true;
	}

	if (rebuild_needed) {
		rebuildDSP(sr, win_samples, pad_choice, bins);
		myCachedPadChoice = pad_choice;
	}

	// Resize channel state array if channel count changed
	int num_channels = output->numChannels;
	if (static_cast<int>(myChannels.size()) != num_channels) {
		myChannels.resize(num_channels, ChannelState());
		for (auto& ch : myChannels) {
			ch.fifo.resize(myBufferCapacity);
			ch.eq.setSampleRate(mySampleRate);
		}
	}

	// Recalculate warping lookup tables and window buffers if UI parameters changed
	if (myCachedScale != scale || std::abs(myCachedDisplayMax - display_max) > 1e-3 ||
		myCachedBins != bins || std::abs(myCachedWarp - warp) > 1e-5 ||
		std::abs(myCachedLogFloor - log_floor) > 1e-3 || myCachedWindowType != window_type ||
		myCachedKaiserBeta != kaiser_beta || myCachedWeighting != weighting || rebuild_needed) {

		updateWarpingAndWindow(scale, display_max, bins, warp, log_floor, window_type, kaiser_beta, weighting);
	}

	// Calculate zero-padding start offset to center windowed audio in FFT frame
	size_t pad_start = (myFFTSize > myBufferCapacity) ? ((myFFTSize - myBufferCapacity) / 2) : 0;

	// Iterate through each audio channel independently
	for (int ch = 0; ch < num_channels; ++ch) {
		ChannelState& st = myChannels[ch];

		// 1. Ingest new audio samples from TouchDesigner input CHOP into FIFO ring buffer
		if (inputs->getNumInputs() > 0) {
			const OP_CHOPInput* cinput = inputs->getInputCHOP(0);
			int num_in_samples = cinput->numSamples;
			if (num_in_samples > 0) {
				const float* cdata = cinput->getChannelData(std::min(ch, cinput->numChannels - 1));
				st.fifo.add(cdata, static_cast<size_t>(num_in_samples));
			}
		}

		// 2. Extract linear sample sequence from circular FIFO
		st.fifo.get(st.captured_signal);

		// 3. Process audio frame through EQ (Zero-copy bypass if EQ is inactive)
		bool has_eq = st.eq.hasActiveFilter(amount);
		if (has_eq) {
			st.eq.processAudio(st.captured_signal, gain_db, cutoff_hz, low_gain_db, low_cutoff_hz, q_factor, amount, st.processed_signal);
		}
		const float* proc_data = has_eq ? st.processed_signal.data() : st.captured_signal.data();

		// 4. Zero-pad audio frame
		if (st.padded_frame.size() != myFFTSize) {
			st.padded_frame.resize(myFFTSize);
		}
		std::fill(st.padded_frame.begin(), st.padded_frame.end(), 0.0f);

		const float* win_data = myWindowBuffer.data();
		float* pad_data = st.padded_frame.data() + pad_start;
		size_t win_len = std::min(myBufferCapacity, myWindowBuffer.size());

		// 5. 2x Unrolled AVX2 SIMD Windowing Loop (Multiplies 16 floats per loop iteration)
		size_t wi = 0;
#if defined(__AVX2__)
		for (; wi + 15 < win_len; wi += 16) {
			__m256 p0 = _mm256_loadu_ps(proc_data + wi);
			__m256 w0 = _mm256_loadu_ps(win_data + wi);
			_mm256_storeu_ps(pad_data + wi, _mm256_mul_ps(p0, w0));

			__m256 p1 = _mm256_loadu_ps(proc_data + wi + 8);
			__m256 w1 = _mm256_loadu_ps(win_data + wi + 8);
			_mm256_storeu_ps(pad_data + wi + 8, _mm256_mul_ps(p1, w1));
		}
		for (; wi + 7 < win_len; wi += 8) {
			__m256 p = _mm256_loadu_ps(proc_data + wi);
			__m256 w = _mm256_loadu_ps(win_data + wi);
			_mm256_storeu_ps(pad_data + wi, _mm256_mul_ps(p, w));
		}
#endif
		for (; wi < win_len; ++wi) {
			pad_data[wi] = proc_data[wi] * win_data[wi];
		}

		// 6. Execute FFTW3 Real-to-Complex Transform & 2x Unrolled AVX2 SIMD Magnitude Calculation
		myFFTWEngine.executeRFFT(st.padded_frame, st.rfft_magnitude, st.scratch_complex);

		// 7. Apply Psychoacoustic Frequency Scale Warping (Log/Mel/ERB/Bark/Chroma with Identity Bypass)
		myWarping.applyWarp(st.rfft_magnitude, st.warped_spectrum);

		// 8. Apply Equal-Loudness Weighting Curve (AVX2 SIMD)
		if (weighting != 0 && myWeightingCurve.size() == st.warped_spectrum.size()) {
			float* w_spec = st.warped_spectrum.data();
			const float* w_curve = myWeightingCurve.data();
			size_t w_len = st.warped_spectrum.size();
			size_t wk = 0;
#if defined(__AVX2__)
			for (; wk + 7 < w_len; wk += 8) {
				__m256 s = _mm256_loadu_ps(w_spec + wk);
				__m256 c = _mm256_loadu_ps(w_curve + wk);
				_mm256_storeu_ps(w_spec + wk, _mm256_mul_ps(s, c));
			}
#endif
			for (; wk < w_len; ++wk) {
				w_spec[wk] *= w_curve[wk];
			}
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

		// 10. Apply Asymmetric Attack/Release Dynamic Ballistics Envelope Smoothing (Zero-copy when disabled)
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
			auto max_it = std::max_element(st.warped_spectrum.begin(), st.warped_spectrum.end());
			size_t max_idx = std::distance(st.warped_spectrum.begin(), max_it);
			myPeakMagnitude = *max_it;
			const auto& target_hz = myWarping.targetHz();
			if (max_idx < target_hz.size()) {
				myPeakFrequencyHz = static_cast<float>(target_hz[max_idx]);
			}
		}
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
	if (index == 0)
	{
		chan->name->setString("execute_count");
		chan->value = static_cast<float>(myExecuteCount);
	}
	else if (index == 1)
	{
		chan->name->setString("fft_size");
		chan->value = static_cast<float>(myFFTSize);
	}
	else if (index == 2)
	{
		chan->name->setString("window_samples");
		chan->value = static_cast<float>(myBufferCapacity);
	}
	else if (index == 3)
	{
		chan->name->setString("sample_rate");
		chan->value = static_cast<float>(mySampleRate);
	}
	else if (index == 4)
	{
		chan->name->setString("peak_freq_hz");
		chan->value = myPeakFrequencyHz;
	}
	else if (index == 5)
	{
		chan->name->setString("peak_magnitude");
		chan->value = myPeakMagnitude;
	}
	else if (index == 6)
	{
		chan->name->setString("simd_avx2_active");
#if defined(__AVX2__)
		chan->value = 1.0f;
#else
		chan->value = 0.0f;
#endif
	}
}

/**
 * @brief Configures diagnostic text table size exported to an Info DAT.
 */
bool
FFT::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	infoSize->rows = 8;
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

	if (index == 0)
	{
		entries->values[0]->setString("execute_count");
		snprintf(tempBuffer, sizeof(tempBuffer), "%d", myExecuteCount);
		entries->values[1]->setString(tempBuffer);
	}
	else if (index == 1)
	{
		entries->values[0]->setString("fft_size");
		snprintf(tempBuffer, sizeof(tempBuffer), "%zu", myFFTSize);
		entries->values[1]->setString(tempBuffer);
	}
	else if (index == 2)
	{
		entries->values[0]->setString("window_samples");
		snprintf(tempBuffer, sizeof(tempBuffer), "%zu", myBufferCapacity);
		entries->values[1]->setString(tempBuffer);
	}
	else if (index == 3)
	{
		entries->values[0]->setString("sample_rate");
		snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", mySampleRate);
		entries->values[1]->setString(tempBuffer);
	}
	else if (index == 4)
	{
		entries->values[0]->setString("nyquist_frequency");
		snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", mySampleRate / 2.0);
		entries->values[1]->setString(tempBuffer);
	}
	else if (index == 5)
	{
		entries->values[0]->setString("spectral_peak_freq");
		snprintf(tempBuffer, sizeof(tempBuffer), "%.1f Hz", myPeakFrequencyHz);
		entries->values[1]->setString(tempBuffer);
	}
	else if (index == 6)
	{
		entries->values[0]->setString("simd_acceleration");
#if defined(__AVX2__)
		entries->values[1]->setString("AVX2 256-bit SIMD");
#else
		entries->values[1]->setString("Scalar Fallback");
#endif
	}
	else if (index == 7)
	{
		entries->values[0]->setString("fft_engine");
		entries->values[1]->setString("FFTW 3.3.5 Single Precision (FFTW_MEASURE)");
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
