#ifndef FFT_H
#define FFT_H

/**
 * ===========================================================================
 *             TOUCHDESIGNER CUSTOM CHOP OPERATOR INTERFACE
 * ===========================================================================
 * Header File: FFT.h
 * Class Definition: FFT (inherits CHOP_CPlusPlusBase, C++ API 10)
 *
 * Real-time architecture (v2.4; v2.8 removed the remaining knobs and left only the async worker):
 *
 *   cook thread (TouchDesigner)                 worker thread (Async = on, default)
 *   ----------------------------------          -----------------------------------
 *   getOutputInfo: poll params (every cook)     wait for a wake-up
 *   execute:                                    poll the job slot every 2 ms (waitFor);
 *     mix / EQ / FIFO ingest            ------> acquire the latest job (TripleBuffer)
 *     write job into a free slot, publish       window -> FFT -> magnitude -> warp
 *     (no lock, no wake-up call while the       -> weighting -> dB -> ballistics -> peak
 *      worker is hot)                           publish the result (TripleBuffer)
 *     acquire the latest result (no lock)       after 500 ms without a job: go dormant
 *     memcpy spectrum -> output                 (sleep until the cook signals once)
 *
 *   Hard real-time rules for the cook thread (Async on):
 *     - no mutex, no condition variable, no kernel call, no allocation after warm-up,
 *       no Python. Two exceptions, both conditional: one WakeByAddressSingle, and only
 *       when the worker had gone dormant; and the deferred Textport log, which takes the
 *       log's mutex and builds a string, but only on a cook where the worker logged a plan
 *       event (a rare, plan-rebuild-time occurrence, never a steady-state one).
 *     - both handoffs are wait-free triple buffers (one atomic exchange each side),
 *       "latest wins", never torn;
 *     - if no new result is ready the previous spectrum is re-output (hold).
 *   The AnalysisPipeline (FFT engine, tables, per-channel DSP state) is entered by ONE
 *   thread at a time: the worker when Async is on, the cook thread when it is off. That is
 *   ownership, not single-threadedness — inside one process() the channel loop fans out over
 *   std::execution::par, and each thread then touches only its own channel's DspState plus
 *   read-only tables.
 *   Telemetry strings are refreshed by the pipeline owner only when they change and
 *   are read by the Info CHOP/DAT callbacks under their own mutex.
 *
 *   Mono first: "Channels = Mono Mix" (default) averages every input channel
 *   into a single analysis channel, so a stereo input costs one FFT.
 *
 *   Threads: the node itself owns exactly one background thread (the async analysis worker
 *   above), and that is the whole of its threading. With more than one analysis channel the
 *   channel loop is also the only intra-cook parallelism, so it runs on std::execution::par
 *   (unconditionally — a one-channel cook has nothing to fan out and stays serial). FFTW cannot
 *   split a single 1-D transform: measured, nthreads > 1 makes one transform 13-51 % slower, not
 *   faster, which is why there is no FFT-threads control and no reason to want one.
 * ===========================================================================
 */

#include "CHOP_CPlusPlusBase.h"
#include "DSPModules.h"
#include "Parameters.h"

#include <atomic>
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
// Snapshot handed from the cook thread to the pipeline (one TripleBuffer slot)
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
// Result handed from the pipeline to the cook thread (one TripleBuffer slot)
// ---------------------------------------------------------------------------------------------
struct AnalysisResult {
    uint64_t seq{ 0 };                            // job this result belongs to (0 = none yet)
    float peakHz{ 0.0f };                         // channel 0 spectral peak (computed by the pipeline owner)
    float peakMag{ 0.0f };
    std::vector<FFTDSP::AlignedVector> spectra;   // per channel: p.bins floats
};

// ---------------------------------------------------------------------------------------------
// Everything after the FIFO. Owned by exactly one thread at a time.
// ---------------------------------------------------------------------------------------------
class AnalysisPipeline {
public:
    explicit AnalysisPipeline(FFTDSP::PlanLog* log);
    ~AnalysisPipeline();

    // Runs the full analysis for every channel of the job into res (spectra resized to p.bins, peak filled).
    void process(const AnalysisJob& job, AnalysisResult& res);

    // Telemetry (owner thread only). statusVersion() changes whenever status() would.
    struct Status {
        std::string plan;
        bool planFailed{ false };          // prepare() could not produce a plan — surface via getErrorString
        size_t fftSize{ 0 }, capacity{ 0 }, linearBins{ 0 }, magnitudeBins{ 0 };
        double axisRate{ 0.0 };              // 2 x (top of the axis) for the grid that was built; NOT info->sampleRate
        double axisBottom{ 0.0 };            // target_hz[0]: DC for Mel/ERB/Linear, the log floor / 20 Hz / ~13 Hz otherwise
        int outputBins{ 0 };
        bool linearGrid{ false };            // the warp came out as the identity: output == linear FFT bins
        bool planUpgrading{ false };
    };
    Status status() const;
    uint64_t statusVersion() const noexcept { return myStatusVersion; }
    double lastUs() const noexcept { return myLastUs; }
    // 2 x the top of the axis read off the current tables (never fitted, never assumed); cheap accessor
    // for the cook thread's publication. This is the SPECTRUM axis rate, not the rate reported to
    // TouchDesigner — that one is FFT::outputSampleRate(), i.e. bins x me.time.rate. See the "Output
    // rate model" block in FFT.cpp.
    double outputSampleRate() const noexcept { return myOutputSampleRate; }
    // True when the current tables are the identity warp, i.e. the output grid IS the linear FFT grid.
    // Derived from the tables themselves (PerceptualWarping::isIdentity), never from a parameter, so it
    // cannot disagree with what applyWarp() actually does.
    bool linearGrid() const noexcept { return myLinearGrid; }
    // Atomic because it crosses threads: written by the pipeline owner (the worker when Async is on,
    // the cook thread when it is off) and read by the Info CHOP/DAT callbacks, which TouchDesigner calls
    // from its own thread. Plain relaxed telemetry — never used to make a decision.
    bool parallelActive() const noexcept { return myParallelActive.load(std::memory_order_relaxed); }
    // True if the engine has no plan ready (owner thread only; read once per published job by FFT::runJob
    // to decide whether to escalate the failure to getErrorString).
    bool planFailed() const noexcept { return !myEngine || !myEngine->hasPlan(); }

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
    // The whole per-channel pipeline (window -> FFT -> magnitude -> warp -> weighting -> dB ->
    // ballistics). Thread-safe: touches only its own DspState plus read-only tables, which is what
    // lets process() fan it out over cores.
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
    uint64_t myStatusVersion{ 1 };
    bool myLastUpgrading{ false };
    size_t myMagnitudeBins{ 0 };
    bool myLinearGrid{ false };             // last built warp is the identity (see linearGrid())
    double myOutputSampleRate{ 0.0 };       // 2 x top of axis; see outputSampleRate() in FFT.cpp
    double myLastUs{ 0.0 };
    std::atomic<bool> myParallelActive{ false };   // last process() fanned out over channels (see above)
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
	void pollParameters(const OP_Inputs* inputs);
	void executeImpl(CHOP_Output* output, const OP_Inputs* inputs);
	void zeroOutputSafe(CHOP_Output* output) noexcept;
	int  analysisChannelCount(const OP_CHOPInput* cinput, Parameters::ChanMode mode) const;
	void ingest(const OP_CHOPInput* cinput, Parameters::ChanMode mode, int numChannels, const Parameters::Values& p, size_t capacity);
	void fillJob(AnalysisJob& job, int numChannels, int winSamples, double dtMs);
	void runJob(const AnalysisJob& job);          // pipeline owner thread (worker, or cook when Async is off)
	void startWorker();
	void stopWorker();
	void workerLoop();
	void copyResultsToOutput(CHOP_Output* output, int numChannels);
	AnalysisPipeline::Status statusSnapshot();

	// Sample rate reported to TouchDesigner for the spectrum: bins * me.time.rate, the rate of one
	// output vector per cook. See the definition in FFT.cpp for what it does and does not carry.
	double outputSampleRate(const Parameters::Values& p) const;

	// The band the axis covers, in "bin 0 is DC, the last bin is Nyquist" form: 2 * (top of axis),
	// clamped to the input rate because Display Max is clamped to Nyquist. Independent of the bin
	// count. This is the axis the frequency data actually lives on, so it is what hzPerSample() and
	// `output_spectrum_axis` are built from — the reported sample rate no longer carries the
	// index-to-Hz mapping.
	double outputAxisRate(const Parameters::Values& p, double sampleRate) const;

	// Hz between consecutive output bins: outputAxisRate()/(2*(bins-1)) = fmax/(bins-1). Exact for
	// every uniform grid (the identity case, Scale = Linear, any blend = 0); for a perceptual grid it
	// is the mean spacing, the only scalar that describes a non-uniform axis. Same name
	// TouchDesigner's own Audio Spectrum CHOP uses for its equivalent Info channel.
	double hzPerSample(const Parameters::Values& p, double sampleRate) const;

	// Samples of spectrum this node produces per second: bins per frame x the rate at which new
	// frames are actually produced. This is data throughput, not
	// the sample rate — see the note above the definition in FFT.cpp.
	double outputBandwidth(const Parameters::Values& p) const;

	const OP_NodeInfo*	myNodeInfo;
	int32_t				myExecuteCount{ 0 };
	bool				myCpuOk{ true };
	std::string			myErrorText;
	int					myExecStage{ 0 };

	// --- parameters (polled in getOutputInfo, every cook) ---
	Parameters::Values	myParams;
	bool				myHaveParams{ false };
	bool				myParamsFreshForExecute{ false };   // set by getOutputInfo, consumed by execute
	int32_t				myParamReads{ 0 };
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
	std::atomic<bool>	myWorkerStop{ false };
	std::atomic<bool>	myWorkerDormant{ false };     // worker sleeps indefinitely; the cook must signal()
	bool				myWorkerRunning{ false };
	FFTDSP::WorkerSignal myWake;
	static constexpr uint32_t kWorkerPollMs = 2;      // job pickup latency while hot (<< one frame)
	static constexpr double   kWorkerDormantAfterMs = 500.0;
	FFTDSP::TripleBuffer<AnalysisJob>    myJobs;      // cook -> pipeline owner
	FFTDSP::TripleBuffer<AnalysisResult> myResults;   // pipeline owner -> cook
	std::atomic<uint64_t> myJobsDropped{ 0 };

	// --- engine/pipeline failure escalation (lock-free; details go to the deferred textport log) ---
	std::atomic<bool>		myPlanFailed{ false };      // prepare() failed to produce a plan for the current size
	std::atomic<uint64_t>	myPipelineErrors{ 0 };      // runJob() caught an exception (slot not published)

	// --- telemetry ---
	std::atomic<double>	myDspUs{ 0.0 };
	std::atomic<double>	myOutputSampleRate{ 0.0 };    // exact spectrum rate read off the pipeline owner's tables
	std::mutex			myStatusMutex;                // Info callbacks <-> pipeline owner; never taken by the cook in async mode
	AnalysisPipeline::Status myStatusCopy;
	uint64_t			myStatusVersionSeen{ 0 };     // pipeline owner only
	double				myLastCookUs{ 0.0 };
	float				myPeakFrequencyHz{ 0.0f };
	float				myPeakMagnitude{ 0.0f };
	std::atomic<double>	myCookDtMs{ 1000.0 / 60.0 };  // last cook's frame time (outputBandwidth); cook writes, Info callbacks read
	std::atomic<double>	myCookRate{ 60.0 };           // me.time.rate (OP_TimeInfo::rate, the timeline FPS where this node lives); cook writes, Info callbacks read
	bool				myAsyncActive{ false };
	int					myAnalysisChannels{ 0 };
	int					myHoldFrames{ 0 };
};

#endif // FFT_H
