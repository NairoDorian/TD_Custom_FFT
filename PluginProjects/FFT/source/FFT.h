#ifndef FFT_H
#define FFT_H

/**
 * ===========================================================================
 *             TOUCHDESIGNER CUSTOM CHOP OPERATOR INTERFACE
 * ===========================================================================
 * Header File: FFT.h
 * Class Definition: FFT (inherits CHOP_CPlusPlusBase, C++ API 10)
 *
 * Real-time architecture (v2.3):
 *
 *   cook thread (TouchDesigner)          worker thread (Async = on, default)
 *   ---------------------------          -----------------------------------
 *   read params (polled)                 wait for a job
 *   mix / EQ / FIFO ingest (~1 us)   -->  window -> FFT -> magnitude -> warp
 *   post AnalysisJob (window copy)       -> weighting -> dB -> ballistics
 *   copy last finished spectrum          publish into the back buffer, swap
 *   to the output (~1.5 us)
 *
 *   The pipeline (AnalysisPipeline) owns the FFT engine, the tables and all
 *   per-channel DSP state and is only ever touched by ONE thread: the worker
 *   when Async is on, the cook thread when it is off. The two threads share a
 *   mailbox (one pending job, latest wins) and a mutex-guarded double buffer
 *   of results. The cook thread never waits for the worker: if no new result
 *   is ready it re-outputs the previous spectrum (hold).
 *
 *   Mono first: "Channels = Mono Mix" (default) averages every input channel
 *   into a single analysis channel, so a stereo input costs one FFT.
 * ===========================================================================
 */

#include "CHOP_CPlusPlusBase.h"
#include "DSPModules.h"
#include "Parameters.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace TD;

// ---------------------------------------------------------------------------------------------
// Cook-thread per-channel ingest state (FIFO, EQ at ingest, silence tracking)
// ---------------------------------------------------------------------------------------------
struct IngestState {
    FFTDSP::FIFOBuffer fifo;
    FFTDSP::BiquadEQ eq;
    FFTDSP::AlignedVector block;       // scratch for the incoming block (mix / EQ)
    size_t silent_run{ 0 };            // consecutive silent samples ingested
};

// ---------------------------------------------------------------------------------------------
// Snapshot handed from the cook thread to the pipeline
// ---------------------------------------------------------------------------------------------
struct AnalysisJob {
    uint64_t seq{ 0 };
    int numChannels{ 0 };
    double sampleRate{ 44100.0 };
    int winSamples{ 3175 };
    double dtMs{ 1000.0 / 60.0 };
    bool reset{ false };
    Parameters::Values p;
    std::vector<FFTDSP::AlignedVector> windows;   // per channel: linearized FIFO (winSamples floats)
    std::vector<uint8_t> silent;                  // per channel: whole window is digital silence
};

// ---------------------------------------------------------------------------------------------
// Everything after the FIFO. Owned by exactly one thread at a time.
// ---------------------------------------------------------------------------------------------
class AnalysisPipeline {
public:
    explicit AnalysisPipeline(FFTDSP::PlanLog* log);
    ~AnalysisPipeline();

    // Runs the full analysis for every channel of the job; out[ch] is resized to p.bins.
    void process(const AnalysisJob& job, std::vector<FFTDSP::AlignedVector>& out);

    // Telemetry (valid on the owning thread; the FFT class copies it under its result mutex)
    struct Status {
        std::string plan;
        size_t fftSize{ 0 }, capacity{ 0 }, linearBins{ 0 }, magnitudeBins{ 0 };
        double dspUs{ 0.0 };
        bool planUpgrading{ false };
    };
    Status status() const;
    const std::vector<double>& targetHz() const { return myWarping.targetHz(); }
    uint64_t tablesVersion() const { return myTablesVersion; }

private:
    struct DspState {
        FFTDSP::AlignedVector padded_frame, rfft_magnitude, prev_spectrum;
        FFTDSP::AlignedComplexVector scratch_complex;
        int prev_loudness_mode{ -1 };
        float agc_peak{ 0.0f };
    };
    struct WindowKey { int type{ -1 }; double beta{ -1.0 }; size_t len{ 0 }; int norm{ -1 };
                       bool operator==(const WindowKey& o) const { return type == o.type && beta == o.beta && len == o.len && norm == o.norm; } };
    struct WarpKey   { int scale{ -1 }; double fmax{ -1.0 }; int bins{ -1 }; double warp{ -1.0 }; double floor{ -1.0 }; size_t nlin{ 0 }; double nyquist{ -1.0 }; int interp{ -1 };
                       bool operator==(const WarpKey& o) const { return scale == o.scale && fmax == o.fmax && bins == o.bins && warp == o.warp && floor == o.floor && nlin == o.nlin && nyquist == o.nyquist && interp == o.interp; } };
    struct WeightKey { int weighting{ -1 }; uint64_t warpVersion{ 0 };
                       bool operator==(const WeightKey& o) const { return weighting == o.weighting && warpVersion == o.warpVersion; } };

    void rebuild(const AnalysisJob& job);
    void updateWindow(const Parameters::Values& p);
    void updateWarp(const Parameters::Values& p);
    void updateWeighting(const Parameters::Values& p);
    void runChannel(DspState& st, const FFTDSP::AlignedVector& window_in, bool silent, const Parameters::Values& p,
                    float attackCoef, float releaseCoef, float agcDecay, FFTDSP::AlignedVector& out) noexcept;

    FFTDSP::PlanLog* myLog;
    std::unique_ptr<FFTDSP::FFTWEngine> myEngine;
    FFTDSP::PerceptualWarping myWarping;
    FFTDSP::AlignedVector myWeightingCurve;
    FFTDSP::AlignedVector myWindowBuffer;
    std::vector<DspState> myChannels;

    double mySampleRate{ 0.0 };
    size_t myCapacity{ 0 };
    size_t myFFTSize{ 0 };
    size_t myPadStart{ 0 };
    int myPadChoice{ -1 };
    FFTDSP::PlannerPolicy myPlanner{ FFTDSP::PlannerPolicy::Auto };
    WindowKey myWindowKey;
    WarpKey myWarpKey;
    WeightKey myWeightKey;
    uint64_t myWarpVersion{ 0 };
    uint64_t myTablesVersion{ 0 };
    size_t myMagnitudeBins{ 0 };
    double myLastUs{ 0.0 };
};

// ---------------------------------------------------------------------------------------------
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
	void executeImpl(CHOP_Output* output, const OP_Inputs* inputs);
	void zeroOutputSafe(CHOP_Output* output) noexcept;
	int  analysisChannelCount(const OP_CHOPInput* cinput, Parameters::ChanMode mode) const;
	void ingest(const OP_CHOPInput* cinput, Parameters::ChanMode mode, int numChannels, const Parameters::Values& p, size_t capacity);
	void publishSync(AnalysisJob& job);
	void startWorker();
	void stopWorker();
	void workerLoop();
	void copyResultsToOutput(CHOP_Output* output, int numChannels);
	void refreshTelemetryLocked();      // called with myResultMutex held

	const OP_NodeInfo*	myNodeInfo;
	int32_t				myExecuteCount{ 0 };
	bool				myCpuOk{ true };
	std::string			myErrorText;
	int					myExecStage{ 0 };

	// --- parameters (polled) ---
	Parameters::Values	myParams;
	bool				myHaveParams{ false };
	int					myParamReads{ 0 };
	double				myParamUs{ 0.0 };
	bool				myResetPending{ false };

	// --- ingest (cook thread) ---
	std::vector<IngestState> myIngest;
	size_t				myCapacity{ 3175 };
	double				mySampleRate{ 44100.0 };
	uint64_t			myJobSeq{ 0 };

	// --- pipeline + worker ---
	FFTDSP::PlanLog		myLog;
	std::unique_ptr<AnalysisPipeline> myPipeline;
	std::thread			myWorker;
	std::mutex			myJobMutex;
	std::condition_variable myJobCv;
	AnalysisJob			myMailbox;
	bool				myMailboxFull{ false };
	bool				myWorkerStop{ false };
	bool				myWorkerRunning{ false };
	std::atomic<uint64_t> myJobsDropped{ 0 };

	// --- results (double buffer, guarded by myResultMutex) ---
	std::mutex			myResultMutex;
	std::vector<FFTDSP::AlignedVector> myResults[2];
	int					myFront{ 0 };
	uint64_t			myPublishedSeq{ 0 };
	uint64_t			myTablesVersionSeen{ 0 };
	std::vector<double>	myTargetHzCopy;
	AnalysisPipeline::Status myStatusCopy;

	// --- telemetry ---
	double				myLastCookUs{ 0.0 };
	float				myPeakFrequencyHz{ 0.0f };
	float				myPeakMagnitude{ 0.0f };
	bool				myAsyncActive{ false };
	int					myAnalysisChannels{ 0 };
	int					myHoldFrames{ 0 };
};

#endif // FFT_H
