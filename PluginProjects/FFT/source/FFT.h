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
// AnalysisPipeline (AnalysisJob, AnalysisResult, AnalysisPipeline) — the TD-free DSP pipeline
// core: window -> FFT -> magnitude -> warp -> weighting -> dB -> ballistics. Extracted into its own
// header so it is unit-testable without TouchDesigner or a worker thread. See AnalysisPipeline.h.
// ---------------------------------------------------------------------------------------------
#include "AnalysisPipeline.h"

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
