#ifndef FFT_H
#define FFT_H

/**
 * ===========================================================================
 *             TOUCHDESIGNER CUSTOM CHOP OPERATOR INTERFACE
 * ===========================================================================
 * Header File: FFT.h
 * Class Definition: FFT (inherits CHOP_CPlusPlusBase)
 *
 * TOUCHDESIGNER INTEGRATION ARCHITECTURE & DESIGN JUSTIFICATIONS:
 * ---------------------------------------------------------------------------
 * TouchDesigner CHOPs (Channel Operators) process streaming multi-channel audio data
 * in real-time at interactive frame rates (e.g. 60 Hz or 120 Hz).
 *
 * Architectural Principles:
 * 1. Independent Multi-Channel Processing:
 *    TouchDesigner CHOP inputs can carry arbitrary channel counts (e.g., Mono, Stereo, 5.1).
 *    `ChannelState` maintains isolated per-channel ring buffers, EQ filters, dynamic ballistics,
 *    and FFT scratchpads to prevent inter-channel cross-talk or race conditions.
 *
 * 2. Parameter Caching & Incremental Recalculation:
 *    TouchDesigner evaluates parameters every frame. Cached variables track parameter
 *    states and only trigger DSP updates when UI controls are actually modified.
 *
 * 3. Persistent Zero-Allocation & Zero-Padding Cooking:
 *    `ChannelState::initBuffers()` pre-sizes work vectors and initializes `padded_frame`
 *    with zeros ONCE during setup/rebuild. During real-time `execute()`, only the active windowed
 *    audio slice is written into `padded_frame`, while the zero-padded regions on the left and right
 *    remain zero persistently, eliminating millions of redundant zero-writes per second.
 * ===========================================================================
 */

#include "CHOP_CPlusPlusBase.h"
#include "DSPModules.h"
#include "Parameters.h"
#include <vector>
#include <string>

using namespace TD;

/**
 * @struct ChannelState
 * @brief Complete DSP state container for a single audio channel.
 *
 * DESIGN & PERFORMANCE RATIONALE:
 * - Pre-allocated work vectors (captured_signal, processed_signal, padded_frame, rfft_magnitude,
 *   warped_spectrum, scratch_complex) act as re-usable scratch memory buffers.
 * - Struct memory layout is cache-friendly and isolates channel memory blocks.
 */
struct ChannelState {
    FFTDSP::FIFOBuffer fifo;                             // Pre-allocated circular sample buffer
    FFTDSP::BiquadEQ eq;                                 // Parametric High/Low shelving equalizer
    FFTDSP::BallisticsFilter ballistics;                 // Envelope follower with independent attack/release
    std::vector<float> prev_spectrum;                    // Stored spectrum frame for temporal ballistics smoothing
    std::vector<float> captured_signal;                  // Unrolled sample frame extracted from FIFO
    std::vector<float> processed_signal;                 // Audio output from Biquad EQ stage
    std::vector<float> padded_frame;                     // Zero-padded audio frame ready for FFT transform
    std::vector<float> rfft_magnitude;                   // Linear RFFT magnitude spectrum output
    std::vector<float> warped_spectrum;                  // Re-mapped psychoacoustic frequency spectrum
    std::vector<std::complex<float>> scratch_complex;    // FFTW3 complex output buffer (size: N/2 + 1)
    int prev_loudness_mode{ -1 };                        // Mode history tracker to reset dynamic ranges cleanly

    void initBuffers(size_t windowCapacity, size_t fftSize, size_t numBins) {
        fifo.resize(windowCapacity);
        if (captured_signal.size() != windowCapacity) captured_signal.resize(windowCapacity);
        if (processed_signal.size() != windowCapacity) processed_signal.resize(windowCapacity);
        
        // Zero-fill padded_frame ONCE on initialization or DSP rebuild
        padded_frame.assign(fftSize, 0.0f);

        size_t numComplexBins = fftSize / 2 + 1;
        if (rfft_magnitude.size() != numComplexBins) rfft_magnitude.resize(numComplexBins);
        if (scratch_complex.size() != numComplexBins) scratch_complex.resize(numComplexBins);
        if (warped_spectrum.size() != numBins) warped_spectrum.resize(numBins);
    }
};

/**
 * @class FFT
 * @brief Custom CHOP Operator class deriving from TouchDesigner's CHOP_CPlusPlusBase interface.
 *
 * FUNCTIONAL OVERVIEW:
 * Inherits CPlusPlus API callbacks required by TouchDesigner to instantiate, parameterize,
 * query, and execute real-time FFT spectrum processing.
 */
class FFT : public CHOP_CPlusPlusBase
{
public:
	/**
	 * @brief Constructor: Receives NodeInfo from TouchDesigner during CHOP creation.
	 */
	FFT(const OP_NodeInfo* info);

	/**
	 * @brief Destructor: Destroys FFTW plans cleanly upon CHOP deletion.
	 */
	virtual ~FFT();

	// =======================================================================
	// TouchDesigner Plugin Framework Callbacks
	// =======================================================================

	/**
	 * @brief Returns general CHOP execution flags (cookEveryFrame = true, timeslice = false).
	 */
	virtual void		getGeneralInfo(CHOP_GeneralInfo*, const OP_Inputs*, void* ) override;

	/**
	 * @brief Defines output channel count, output sample count (bins), and sample rate.
	 */
	virtual bool		getOutputInfo(CHOP_OutputInfo*, const OP_Inputs*, void*) override;

	/**
	 * @brief Generates channel names for TouchDesigner (e.g. chan1_fft, chan2_fft).
	 */
	virtual void		getChannelName(int32_t index, OP_String *name, const OP_Inputs*, void* reserved) override;

	/**
	 * @brief Primary real-time execution entry point called by TouchDesigner every frame cook (60-120 FPS).
	 */
	virtual void		execute(CHOP_Output*,
								const OP_Inputs*,
								void* reserved) override;

	// Info CHOP channels & Info DAT entries for diagnostic node inspection in TouchDesigner
	virtual int32_t		getNumInfoCHOPChans(void* reserved1) override;
	virtual void		getInfoCHOPChan(int index,
										OP_InfoCHOPChan* chan,
										void* reserved1) override;

	virtual bool		getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1) override;
	virtual void		getInfoDATEntries(int32_t index,
											int32_t nEntries,
											OP_InfoDATEntries* entries,
											void* reserved1) override;

	virtual void		getInfoPopupString(OP_String *info, void *reserved1) override;
	virtual void		getWarningString(OP_String *warning, void *reserved1) override;
	virtual void		getErrorString(OP_String *error, void *reserved1) override;

	// Parameter setup & Pulse button callback
	virtual void		setupParameters(OP_ParameterManager* manager, void *reserved1) override;
	virtual void		pulsePressed(const char* name, void* reserved1) override;

private:
	const OP_NodeInfo*	myNodeInfo;       // TouchDesigner Node Information handle
	int32_t				myExecuteCount;   // Cumulative frame cook counter

	double				mySampleRate;      // Audio sampling rate in Hz (e.g. 44100.0 or 48000.0)
	size_t				myBufferCapacity;  // FIFO buffer capacity (Window sampling size)
	size_t				myFFTSize;         // Zero-padded FFT transform length (e.g. 32768)
	float				myPeakFrequencyHz{ 0.0f }; // Real-time peak spectral frequency (Hz)
	float				myPeakMagnitude{ 0.0f };   // Real-time peak spectral magnitude / dB

	FFTDSP::PerceptualWarping myWarping;    // Psychoacoustic frequency warping manager
	std::unique_ptr<FFTDSP::IFFTEngine> myFFTEngine; // Polymorphic CPU/GPU FFT execution engine
	std::vector<float>       myWeightingCurve; // Pre-calculated equal-loudness weighting curve
	std::vector<float>       myWindowBuffer;   // Pre-calculated audio tapering window shape

	std::vector<ChannelState> myChannels;  // Per-channel state vector

	// Parameter Caching fields to eliminate redundant DSP recalculations
	int    myCachedEngine{ -1 };
	int    myCachedScale{ -1 };
	double myCachedDisplayMax{ -1.0 };
	int    myCachedBins{ -1 };
	double myCachedWarp{ -1.0 };
	double myCachedLogFloor{ -1.0 };
	int    myCachedWindowType{ -1 };
	int    myCachedKaiserBeta{ -1 };
	int    myCachedWeighting{ -1 };
	int    myCachedPadChoice{ -1 };

	// Crash-guard diagnostics: records which execute() stage was running when an
	// exception was caught, so the failing phase is visible in the plan log / Textport.
	int    myExecStage{ 0 };

	/**
	 * @brief Rebuilds DSP buffer capacities and hardware-benchmarks FFTW plans when FFT size changes.
	 */
	void rebuildDSP(int engineChoice, double sr, int winSamples, int padChoice, int numBins);

	/**
	 * @brief Internal implementation of rebuildDSP (exception-safe wrapper calls this).
	 */
	void rebuildDSPInternal(int engineChoice, double sr, int winSamples, int padChoice, int numBins);

	/**
	 * @brief Updates psychoacoustic warping lookup tables, weighting curves, and window shapes.
	 */
	void updateWarpingAndWindow(int scale, double displayMax, int bins, double warp, double logFloor, int winType, int kaiserBeta, int weighting);

	/**
	 * @brief Body of execute() with stage markers and defensive clamps on host-provided values.
	 */
	void executeImpl(CHOP_Output* output, const OP_Inputs* inputs);

	/**
	 * @brief Zeros the output CHOP within safe bounds after a caught exception.
	 */
	void zeroOutputSafe(CHOP_Output* output) noexcept;
};

#endif // FFT_H
