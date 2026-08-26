#ifndef FFT_H
#define FFT_H

/**
 * ===========================================================================
 *             TOUCHDESIGNER CUSTOM CHOP OPERATOR INTERFACE
 * ===========================================================================
 * Header File: FFT.h
 * Class Definition: FFT (inherits CHOP_CPlusPlusBase, C++ API 10)
 *
 * Architectural principles:
 * 1. Independent multi-channel processing: ChannelState holds per-channel ring
 *    buffer, EQ, ballistics and FFT scratch memory; channels can be processed
 *    in parallel (std::execution::par) because they share only read-only tables.
 * 2. Parameter caching with per-artifact keys: the window, the warp tables and
 *    the weighting curve are each rebuilt only when their own inputs change.
 * 3. Zero allocation in execute(): all buffers are sized in rebuildDSP().
 * 4. Persistent zero padding: padded_frame is zero-filled once; execute() only
 *    writes the windowed slice into the (8-float aligned) centre.
 * ===========================================================================
 */

#include "CHOP_CPlusPlusBase.h"
#include "DSPModules.h"
#include "Parameters.h"

#include <memory>
#include <string>
#include <vector>

using namespace TD;

struct ChannelState {
    FFTDSP::FIFOBuffer fifo;                       // circular sample buffer
    FFTDSP::BiquadEQ eq;                           // high/low shelving EQ
    FFTDSP::BallisticsFilter ballistics;           // attack/release envelope
    FFTDSP::AlignedVector prev_spectrum;           // ballistics state
    FFTDSP::AlignedVector captured_signal;         // linearized FIFO
    FFTDSP::AlignedVector processed_signal;        // EQ output
    FFTDSP::AlignedVector padded_frame;            // zero-padded FFT input
    FFTDSP::AlignedVector rfft_magnitude;          // |X| (N/2+1)
    FFTDSP::AlignedVector warped_spectrum;         // psychoacoustic grid
    FFTDSP::AlignedComplexVector scratch_complex;  // FFTW output (N/2+1)
    int   prev_loudness_mode{ -1 };
    float agc_peak{ 0.0f };                        // slow AGC follower state (dB Reference = Agc)

    void initBuffers(size_t windowCapacity, size_t fftSize, size_t numBins) {
        fifo.resize(windowCapacity);
        if (captured_signal.size() != windowCapacity) captured_signal.resize(windowCapacity);
        if (processed_signal.size() != windowCapacity) processed_signal.resize(windowCapacity);
        padded_frame.assign(fftSize, 0.0f);                   // zero-fill ONCE
        size_t numComplexBins = fftSize / 2 + 1;
        if (rfft_magnitude.size() != numComplexBins) rfft_magnitude.resize(numComplexBins);
        if (scratch_complex.size() != numComplexBins) scratch_complex.resize(numComplexBins);
        if (warped_spectrum.size() != numBins) warped_spectrum.resize(numBins);
        prev_spectrum.clear();
        agc_peak = 0.0f;
        eq.reset();
    }
};

class FFT : public CHOP_CPlusPlusBase
{
public:
	FFT(const OP_NodeInfo* info);
	virtual ~FFT();

	virtual void		getGeneralInfo(CHOP_GeneralInfo*, const OP_Inputs*, void*) override;
	virtual bool		getOutputInfo(CHOP_OutputInfo*, const OP_Inputs*, void*) override;
	virtual void		getChannelName(int32_t index, OP_String* name, const OP_Inputs*, void* reserved) override;
	virtual void		execute(CHOP_Output*, const OP_Inputs*, void* reserved) override;

	virtual int32_t		getNumInfoCHOPChans(void* reserved1) override;
	virtual void		getInfoCHOPChan(int index, OP_InfoCHOPChan* chan, void* reserved1) override;
	virtual bool		getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1) override;
	virtual void		getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1) override;
	virtual void		getInfoPopupString(OP_String* info, void* reserved1) override;
	virtual void		getWarningString(OP_String* warning, void* reserved1) override;
	virtual void		getErrorString(OP_String* error, void* reserved1) override;

	virtual void		setupParameters(OP_ParameterManager* manager, void* reserved1) override;
	virtual void		pulsePressed(const char* name, void* reserved1) override;

private:
	// --- cache keys -------------------------------------------------------------------------
	struct WindowKey { int type{ -1 }; double beta{ -1.0 }; size_t len{ 0 }; int norm{ -1 };
	                   bool operator==(const WindowKey& o) const { return type == o.type && beta == o.beta && len == o.len && norm == o.norm; } };
	struct WarpKey   { int scale{ -1 }; double fmax{ -1.0 }; int bins{ -1 }; double warp{ -1.0 }; double floor{ -1.0 }; size_t nlin{ 0 }; double nyquist{ -1.0 };
	                   bool operator==(const WarpKey& o) const { return scale == o.scale && fmax == o.fmax && bins == o.bins && warp == o.warp && floor == o.floor && nlin == o.nlin && nyquist == o.nyquist; } };
	struct WeightKey { int weighting{ -1 }; uint64_t warpVersion{ 0 };
	                   bool operator==(const WeightKey& o) const { return weighting == o.weighting && warpVersion == o.warpVersion; } };

	const OP_NodeInfo*	myNodeInfo;
	int32_t				myExecuteCount{ 0 };
	bool				myCpuOk{ true };
	bool				myParallelActive{ false };
	double				myLastCookUs{ 0.0 };
	double				myLastParamUs{ 0.0 };   // time spent in Parameters::eval() (TouchDesigner parameter fetches)

	double				mySampleRate{ 44100.0 };
	size_t				myBufferCapacity{ 3175 };
	size_t				myFFTSize{ 32768 };
	size_t				myPadStart{ 0 };
	int					myCachedPadChoice{ -1 };
	FFTDSP::PlannerPolicy myCachedPlanner{ FFTDSP::PlannerPolicy::Auto };
	float				myPeakFrequencyHz{ 0.0f };
	float				myPeakMagnitude{ 0.0f };

	FFTDSP::PerceptualWarping myWarping;
	std::unique_ptr<FFTDSP::IFFTEngine> myFFTEngine;
	FFTDSP::AlignedVector myWeightingCurve;
	FFTDSP::AlignedVector myWindowBuffer;
	std::vector<ChannelState> myChannels;
	FFTDSP::PlanLog		myLog;

	WindowKey			myWindowKey;
	WarpKey				myWarpKey;
	WeightKey			myWeightKey;
	uint64_t			myWarpVersion{ 0 };
	int					myExecStage{ 0 };
	std::string			myErrorText;

	void rebuildDSP(double sr, int winSamples, int padChoice, FFTDSP::PlannerPolicy planner, int numBins);
	void rebuildDSPInternal(double sr, int winSamples, int padChoice, FFTDSP::PlannerPolicy planner, int numBins);
	void updateWindow(const Parameters::Values& p);
	void updateWarp(const Parameters::Values& p);
	void updateWeighting(const Parameters::Values& p);
	void processChannel(ChannelState& st, const OP_CHOPInput* cinput, int ch, CHOP_Output* output,
	                    const Parameters::Values& p, float attackCoef, float releaseCoef, float agcDecay) noexcept;
	void executeImpl(CHOP_Output* output, const OP_Inputs* inputs);
	void zeroOutputSafe(CHOP_Output* output) noexcept;
};

#endif // FFT_H
