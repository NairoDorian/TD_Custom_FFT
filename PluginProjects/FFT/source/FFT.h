#ifndef FFT_H
#define FFT_H

/**
 * ===========================================================================
 *             TOUCHDESIGNER CUSTOM CHOP OPERATOR INTERFACE
 * ===========================================================================
 * Header File: FFT.h
 * Class Definition: FFT (inherits CHOP_CPlusPlusBase, C++ API 10)
 *
 * What this node is, in one paragraph
 * -----------------------------------
 * A CHOP that turns its input audio into a spectrum. It outputs one channel per analysis channel
 * and one sample per output bin, and the value in each sample is the magnitude of that frequency
 * band. It draws nothing and it makes no sound: the output is a value curve, so what turns it into
 * a picture is whatever sits downstream (a CHOP To DAT, a visualiser, a SOP).
 *
 * What this class owns, and what it does not
 * ------------------------------------------
 * This class owns the NODE: the TouchDesigner callbacks, the parameters, the audio ingest, the
 * output writing, the worker thread, and everything the UI reads (Info CHOP, Info DAT, popup,
 * warning and error strings). It does NOT own the DSP. The analysis lives in AnalysisPipeline
 * (AnalysisPipeline.h / AnalysisPipeline.cpp), and the primitives it is built from live in
 * DSPModules.h. Every function declared in this file is implemented in FFT.cpp.
 *
 * Who includes this file
 * ----------------------
 * FFT.cpp, and nothing else. The unit tests (tests/dsp_tests.cpp) and the bench (bench/bench.cpp)
 * deliberately include DSPModules.h, RateModel.h and AnalysisPipeline.h instead, so they can drive
 * the engine with no TouchDesigner, no node and no worker thread. The practical consequence:
 * changes in this file are not covered by the test suite, and changes in those three files are.
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
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace TD;

// ---------------------------------------------------------------------------------------------
// Cook-thread per-channel ingest state (FIFO, EQ at ingest, silence tracking)
// ---------------------------------------------------------------------------------------------
// WHAT: everything one input channel has to remember from one cook to the next while its audio is
// being fed into the FFT window.
// WHY it must be a member of the node (one of these per channel, in myIngest) and not a local in
// ingest(): a cook does not receive a whole FFT window. It receives however many samples arrived
// since the last cook, and the window spans many cooks, so the FIFO has to survive between them.
// Storing it on the stack would reset the stream every frame and the spectrum would be a slice of
// one frame's audio rather than a window of the last N samples.
// WHY an EQ here at all: the EQ is applied at ingest rather than to the output because it is an
// input-shaping filter - it has to be inside the analysed signal, and being one biquad state per
// channel is one multiply-add per sample on the cook thread, which is affordable at ingest.
// HOW TO CHANGE: adding a field is fine as long as it is per-channel state that must persist. Do
// not put anything here that is shared between channels or between threads - this struct is touched
// only by the cook thread, and only one channel's copy at a time.
// USED BY: FFT::ingest() and FFT::fillJob() in FFT.cpp. See also the note on myIngest below.
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
// The six steps of one cook, and which file implements each
// ---------------------------------------------------------------------------------------------
// This is the shortest accurate map of the plugin. Step 4 is the only one that is DSP; the other
// five are the node's job of moving audio in, a spectrum out, and a story about it to the UI.
//
//   1. Bookkeeping and parameters. Measure the gap since the last cook, poll the parameters (or
//      reuse the ones getOutputInfo already polled), read the input sample rate and the timeline
//      rate.                                        -> FFT.cpp (executeImpl, getOutputInfo, pollParameters)
//   2. Ingest. Mix or select channels, apply the EQ, push the new samples into each channel's FIFO.
//                                                   -> FFT.cpp (ingest), using FFTDSP::FIFOBuffer and
//                                                      FFTDSP::BiquadEQ from DSPModules.h
//   3. Hand the work off. Build an AnalysisJob from each channel's window and publish it through a
//      lock-free triple buffer; wake the worker only if it had gone dormant. With Async off, run it
//      inline on the cook thread instead.
//                                                   -> FFT.cpp (fillJob, runJob, startWorker,
//                                                      stopWorker, workerLoop), using
//                                                      FFTDSP::TripleBuffer and FFTDSP::WorkerSignal
//                                                      from DSPModules.h
//   4. The analysis itself, per channel: window -> FFT -> magnitude -> full-scale normalize ->
//      warp -> weighting -> dB -> ballistics.       -> AnalysisPipeline.cpp (process, runChannel,
//                                                      rebuild, updateWindow/Warp/Weighting), built
//                                                      on the tables and filters in DSPModules.h
//   5. Output. Copy the newest published spectrum into the CHOP, or re-output the previous one when
//      nothing new has arrived (hold).             -> FFT.cpp (copyResultsToOutput)
//   6. Report. Refresh the memoized status snapshot, then let the Info CHOP, Info DAT, popup and
//      warning/error callbacks read it; flush any deferred log lines to the Textport.
//                                                   -> FFT.cpp (statusSnapshot, the getInfo*
//                                                      callbacks), using FFTDSP::PlanLog from
//                                                      DSPModules.h
//
// Threading, in one line: steps 1, 2, 5 and 6 are the cook thread, step 3 is either thread, and
// step 4 is whichever thread owns the pipeline right now - the worker when Async is on, the cook
// thread when it is off. Only one thread is ever inside step 4 for a given node.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
class FFT : public CHOP_CPlusPlusBase
{
public:
	FFT(const OP_NodeInfo* info);
	virtual ~FFT();

	// The two callbacks TouchDesigner uses to drive the node. Both are implemented in FFT.cpp.
	// getOutputInfo runs before execute in the same cook and is where the parameters are read, so that
	// execute does not pay for the parameter reads a second time (see myParamsFreshForExecute below).
	virtual void		getGeneralInfo(CHOP_GeneralInfo*, const OP_Inputs*, void*) override;
	virtual bool		getOutputInfo(CHOP_OutputInfo*, const OP_Inputs*, void*) override;
	// getChannelName: the name shown on each output channel. A presentation detail only - nothing in
	// the pipeline reads it back, so it can be changed freely without touching the DSP.
	virtual void		getChannelName(int32_t index, OP_String* name, const OP_Inputs*, void* reserved) override;
	// execute: the cook itself - see "the six steps of one cook" above. This is the entry point the
	// real-time rules in the file header apply to. It wraps executeImpl in a try/catch that reports
	// the failing stage and zeroes the output rather than letting an exception escape into
	// TouchDesigner; if you add work to the cook, it goes in executeImpl, under that same protection.
	virtual void		execute(CHOP_Output*, const OP_Inputs*, void* reserved) override;

	// The information surface. TouchDesigner calls these one item at a time: the Info CHOP channel
	// count, then each channel, then the Info DAT size, then each row, then the popup, then the
	// warning and error strings. All of them run inside the cook, so they are on the real-time path,
	// and all of them read the memoized status snapshot rather than the pipeline (see
	// statusSnapshot() below). Changing what any of them reports is a UI change only - nothing here
	// feeds back into the analysis.
	//
	// getWarningString and getErrorString are the two that TouchDesigner also uses to decide whether
	// the node is "in error", which changes what the node reports at all - so an error string left
	// latched does more than display a message (see myErrorText and myPipelineFailing).
	virtual int32_t		getNumInfoCHOPChans(void* reserved1) override;
	virtual void		getInfoCHOPChan(int index, OP_InfoCHOPChan* chan, void* reserved1) override;
	virtual bool		getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1) override;
	virtual void		getInfoDATEntries(int32_t index, int32_t nEntries, OP_InfoDATEntries* entries, void* reserved1) override;
	virtual void		getInfoPopupString(OP_String* info, void* reserved1) override;
	virtual void		getWarningString(OP_String* warning, void* reserved1) override;
	virtual void		getErrorString(OP_String* error, void* reserved1) override;

	// setupParameters: builds the parameter pages. It runs once, at COMP creation, not per cook, and
	// it is where the Planner Policy and FFT Backend controls are declared. The backend control's
	// menu values must stay in step with the FftBackend.h registry - see the static_asserts in
	// Parameters.cpp, which are what enforce that.
	virtual void		setupParameters(OP_ParameterManager* manager, void* reserved1) override;
	// pulsePressed: the Reset button. It only sets a flag (myResetPending) that the next cook acts
	// on, because a pulse can arrive outside the cook and the pipeline's state belongs to its owner
	// thread. HOW TO CHANGE: keep it to setting a flag - doing real work here would be work done on
	// TouchDesigner's thread at an arbitrary moment.
	virtual void		pulsePressed(const char* name, void* reserved1) override;

private:
	// --- cook-thread helpers (steps 1, 2 and 5 of one cook; all implemented in FFT.cpp) ---
	// pollParameters: copies the scene's parameter values into myParams.
	void pollParameters(const OP_Inputs* inputs);
	// executeImpl: the cook itself, with no try/catch of its own - execute() supplies that. It also
	// keeps myExecStage up to date so an exception can be reported with the stage it happened in.
	// This is the one function to read first in FFT.cpp.
	void executeImpl(CHOP_Output* output, const OP_Inputs* inputs);
	// zeroOutputSafe: writes zeros over every output channel. noexcept and null-checked, because its
	// job is to be the thing that always works - it is called from the exception handlers in
	// execute(), where throwing again would be fatal.
	void zeroOutputSafe(CHOP_Output* output) noexcept;
	// analysisChannelCount: how many channels the analysis runs on, from the ChanMode parameter and
	// the input's channel count. Mono Mix (the default) answers 1 for any input.
	int  analysisChannelCount(const OP_CHOPInput* cinput, Parameters::ChanMode mode) const;
	// ingest: step 2 - mix or pick channels, run the EQ, push samples into each FIFO. Called once per
	// cook, on the cook thread, and it must stay allocation-free in the steady state.
	void ingest(const OP_CHOPInput* cinput, Parameters::ChanMode mode, int numChannels, const Parameters::Values& p, size_t capacity);
	// fillJob: builds the AnalysisJob for this cook - per channel, the window of samples, plus the
	// rates and parameters the pipeline needs. Note it reads from the FIFOs that ingest() just wrote.
	void fillJob(AnalysisJob& job, int numChannels, int winSamples, double dtMs);
	// runJob: step 4's outer wrapper - runs the pipeline for one job and turns its outcome into
	// records (published result, failure flags, status snapshot). Called on the pipeline owner
	// thread: the worker when Async is on, the cook thread when it is off.
	void runJob(const AnalysisJob& job);          // pipeline owner thread (worker, or cook when Async is off)
	// startWorker/stopWorker: create and join the one background thread. called from executeImpl when
	// the Async parameter differs from myWorkerRunning, so the thread's whole lifetime is decided
	// there. stopWorker blocks until the thread has left runJob, which is why toggling Async is the
	// one parameter change that can cost a frame.
	void startWorker();
	void stopWorker();
	// workerLoop: the thread body - poll the job slot while hot, sleep when dormant. Never returns
	// until myWorkerStop is set.
	void workerLoop();
	// copyResultsToOutput: step 5 - copy the newest spectrum into the CHOP, or hold the previous one.
	// It also records the peak and the hold-frame count that the Info CHOP reports.
	void copyResultsToOutput(CHOP_Output* output, int numChannels);
	// The status struct, memoized. Returns a reference to a copy that is refreshed only when the published
	// one moves on (see myStatusPubVersion). Callers must bind it by reference: TouchDesigner asks for
	// this once per Info CHOP channel and once per Info DAT row, so a by-value return here would put the
	// two std::string copies back on every one of those ~300 calls per cook.
	const AnalysisPipeline::Status& statusSnapshot();

	// Sample rate reported to TouchDesigner for the spectrum: bins * me.time.rate, the rate of one
	// output vector per cook. See the definition in FFT.cpp for what it does and does not carry.
	// --- the four different "rates" this node reports, and what each one is for ---
	// They are easy to confuse because they are all "a rate about the spectrum". Named here so the
	// right one gets used: outputSampleRate is what TouchDesigner is told the channel's sample rate
	// is, outputAxisRate and hzPerSample describe the frequency axis the bins sit on, and
	// outputBandwidth is how many numbers per second the node emits. All four are read by the Info
	// CHOP and DAT; none of them affects a sample value.
	//
	// outputSampleRate: bins * me.time.rate - the rate of one whole spectrum per frame.
	// CALLED BY: getOutputInfo (to declare the channel's rate) and the Info callbacks.
	double outputSampleRate(const Parameters::Values& p) const;

	// The band the axis covers, in "bin 0 is DC, the last bin is Nyquist" form: 2 * (top of axis),
	// clamped to the input rate because Display Max is clamped to Nyquist. Independent of the bin
	// count. This is the axis the frequency data actually lives on, so it is what hzPerSample() and
	// `output_spectrum_axis` are built from — the reported sample rate no longer carries the
	// index-to-Hz mapping.
	// CALLED BY: hzPerSample() below, outputBandwidth(), and the Info DAT rows that report the axis.
	double outputAxisRate(const Parameters::Values& p, double sampleRate) const;

	// Hz between consecutive output bins: outputAxisRate()/(2*(bins-1)) = fmax/(bins-1). Exact for
	// every uniform grid (the identity case, Scale = Linear, any blend = 0); for a perceptual grid it
	// is the mean spacing, the only scalar that describes a non-uniform axis. Same name
	// TouchDesigner's own Audio Spectrum CHOP uses for its equivalent Info channel.
	// CALLED BY: the Info CHOP's hz_per_sample channel and the Info DAT. This is the number that
	// answers "what frequency is bin n?", which the reported sample rate no longer carries.
	double hzPerSample(const Parameters::Values& p, double sampleRate) const;

	// Samples of spectrum this node produces per second: bins per frame x the rate at which new
	// frames are actually produced. This is data throughput, not
	// the sample rate — see the note above the definition in FFT.cpp.
	// CALLED BY: the Info CHOP's bandwidth channel. It is a throughput figure, so it moves when the
	// cook rate moves - which is exactly what makes it useful in the popup during a stall.
	double outputBandwidth(const Parameters::Values& p) const;

	// --- node identity and cook state (cook thread) ---
	// myNodeInfo: the description of the node TouchDesigner passes to the constructor. It is owned by
	// TouchDesigner, so it is read and never freed here.
	const OP_NodeInfo*	myNodeInfo;
	// myExecuteCount: how many cooks this instance has run. Reported so that "the node is idle" and
	// "the node is stalled" can be told apart from outside.
	int32_t				myExecuteCount{ 0 };
	// myCpuOk: false when this CPU cannot run the vendored FFTW build (it has no AVX2/FMA). It is set
	// once, in the constructor, from the process-wide g_cpuHasAVX2 probe, and never changes after -
	// which is why it is a plain bool and not an atomic. When it is false, executeImpl zeroes the
	// output and returns before touching the pipeline, so this is the one thing that can make the node
	// produce silence on purpose. HOW TO CHANGE: it is also reported as simd_avx2_active (Info CHOP)
	// and simd_acceleration (Info DAT), so keep those consistent if the condition ever changes.
	bool				myCpuOk{ true };
	// myErrorText: the latched reason the node is currently in error; empty when it is not. Cleared by
	// the next cook that completes (see execute()), and surfaced through getErrorString(). Both ends
	// of that lifetime are deliberate - read the comment in execute() before changing either.
	std::string			myErrorText;
	// myExecStage: which numbered step of executeImpl is currently running, 0 when not cooking. Its
	// only purpose is to make an exception report name the stage it happened in, so keep it updated if
	// you add or reorder steps.
	int					myExecStage{ 0 };

	// --- parameters (polled in getOutputInfo, every cook) ---
	// myParams: the last-read parameter values, as one struct, so the DSP never goes back to
	// TouchDesigner for anything. getOutputInfo fills it; execute reads it.
	Parameters::Values	myParams;
	// myHaveParams: false until the first poll succeeds (the parameter manager is not readable at the
	// very first call). Gates anything that would otherwise read an all-zero myParams.
	bool				myHaveParams{ false };
	// myParamsFreshForExecute: the handshake that keeps the parameter reads to once per cook. Set by
	// getOutputInfo, consumed and cleared by executeImpl. If executeImpl finds it false (execute
	// called without getOutputInfo, which TouchDesigner allows) it polls for itself - that is the
	// whole reason both functions call pollParameters().
	bool				myParamsFreshForExecute{ false };   // set by getOutputInfo, consumed by execute
	// myParamReads / myParamUs: how many parameter values were read and how long it took, reported in
	// the Info DAT. Telemetry only - nothing branches on them.
	int32_t				myParamReads{ 0 };
	double				myParamUs{ 0.0 };
	// myResetPending: set by pulsePressed() (the Reset button), consumed by the next cook, which turns
	// it into job.reset. It is a flag rather than an action because a pulse can arrive between cooks
	// and the state it clears belongs to the pipeline owner thread.
	bool				myResetPending{ false };

	// --- ingest (cook thread) ---
	// myIngest: one IngestState per analysis channel - the FIFOs, EQs and silence counters that carry
	// the audio stream across cooks. Resized to the analysis channel count, never to the input's
	// channel count (Mono Mix collapses N inputs to one analysis channel).
	std::vector<IngestState> myIngest;
	// myCapacity: the number of samples in one FFT window - the transform length, not the input's
	// block size. Written once per cook from the Pad parameter and the sample rate, and then used as
	// the FIFO depth and the job's window length. It is the single figure that ties window, transform
	// and output bin count together, so a change to how it is computed changes all three.
	size_t				myCapacity{ 3175 };
	// mySampleRate: the input's sample rate, clamped to Parameters::kMinSampleRate/kMaxSampleRate.
	// Written once per cook by executeImpl and read by the window-length calculation and the Info
	// callbacks. The clamp is defensive: a bad rate from an upstream node would otherwise make the
	// window length absurd or zero.
	double				mySampleRate{ 44100.0 };
	// myJobSeq: the cook's own counter, incremented per published job. A result carries the job's seq,
	// and the difference between the two is what copyResultsToOutput turns into myHoldFrames - i.e.
	// "how many cooks behind is the spectrum I am showing".
	uint64_t			myJobSeq{ 0 };

	// --- pipeline + worker ---
	// myLog: the log every part of the plugin writes to. In deferred mode the worker only queues a
	// string (no Python, no Textport call from a background thread); the cook flushes it. PlanLog and
	// the reason for deferred mode are documented in DSPModules.h.
	FFTDSP::PlanLog		myLog;
	// myPipeline: the DSP engine. Owned by the node, but only ONE thread at a time may be inside it -
	// the worker when Async is on, the cook thread when it is off. That is ownership, not
	// single-threadedness; see the file header.
	std::unique_ptr<AnalysisPipeline> myPipeline;
	// myWorker: the one background thread this node owns. Its lifetime is decided entirely by the
	// Async parameter, toggled in executeImpl - see startWorker/stopWorker.
	std::thread			myWorker;
	// myWorkerStop: the worker's exit request. Set by stopWorker, read at the top of workerLoop.
	std::atomic<bool>	myWorkerStop{ false };
	// myWorkerDormant: the worker has stopped polling and is asleep until the cook signals. The cook
	// must call myWake.signal() when this is true, or the job it just published is never picked up -
	// that handshake is the most delicate part of the node, and it is documented in workerLoop().
	std::atomic<bool>	myWorkerDormant{ false };     // worker sleeps indefinitely; the cook must signal()
	// myWorkerRunning: true while a thread exists. Cook thread only - it is what executeImpl compares
	// the Async parameter against to decide whether to start or stop the worker. Deliberately NOT
	// atomic: it is never read from the worker.
	bool				myWorkerRunning{ false };
	// myWake: the wait/wake primitive the worker sleeps on. A plain condition variable would be a
	// forbidden kernel call on the hot path, which is why this type exists (see DSPModules.h).
	FFTDSP::WorkerSignal myWake;
	// The two timings of the worker's sleep policy: poll every kWorkerPollMs while jobs are flowing
	// (cheap, no kernel call), and go dormant only after kWorkerDormantAfterMs with nothing to do.
	// Increasing the poll interval adds latency to every spectrum update; decreasing it burns CPU on a
	// node that is otherwise idle. Changing the dormancy threshold trades the one-off wake-up cost
	// against the cost of polling forever.
	static constexpr uint32_t kWorkerPollMs = 2;      // job pickup latency while hot (<< one frame)
	static constexpr double   kWorkerDormantAfterMs = 500.0;
	// The two lock-free handoffs. Each is "latest wins": a new job overwrites an unread one rather
	// than queueing, which is correct here because only the newest audio window is worth analysing.
	// HOW TO CHANGE: the types are templated on the payload; if either payload gains a member that is
	// not trivially copyable, the handoff stops being valid - see TripleBuffer in DSPModules.h.
	FFTDSP::TripleBuffer<AnalysisJob>    myJobs;      // cook -> pipeline owner
	FFTDSP::TripleBuffer<AnalysisResult> myResults;   // pipeline owner -> cook
	// myJobsDropped: how many times a job was published before the worker had taken the previous one.
	// Reported as async_jobs. It is a health number - steady non-zero means the analysis cannot keep
	// up with the cook rate, which is worth seeing rather than hiding.
	std::atomic<uint64_t> myJobsDropped{ 0 };

	// --- engine/pipeline failure escalation (lock-free; details go to the deferred textport log) ---
	// Two different questions are answered here, and they are deliberately separate fields:
	//   myPlanFailed / myPipelineErrors  - "has anything gone wrong?" (a fact about the past), and
	//   myPipelineFailing               - "is it going wrong NOW?" (a fact about the present).
	// The error string is driven by the second one only, because a lifetime tally can never come back
	// down and would leave a healthy node flagged as broken forever. Everything here is atomic and
	// written by the pipeline owner thread, read by the cook and the Info callbacks.
	//
	// myPlanFailed: the engine has no usable plan for the CURRENT window size - usually a plan that
	// could not be created, or one being rebuilt. Resets by itself when a plan exists again (runJob
	// rewrites it from the pipeline after every analysis), so unlike the others it can go back to
	// false without any exception having to be recovered from.
	std::atomic<bool>		myPlanFailed{ false };      // prepare() failed to produce a plan for the current size
	// myPipelineErrors: the LIFETIME count of exceptions thrown by runJob(). Telemetry only - it is
	// reported in the Info DAT and is never used to decide anything. Do not make the error string
	// depend on it; that is what myPipelineFailing is for.
	std::atomic<uint64_t>	myPipelineErrors{ 0 };      // lifetime count of runJob() exceptions, telemetry only
	// Whether the MOST RECENT analysis threw. The lifetime counter above is not a statement about the
	// present, so it must not drive the error string on its own: latched on the counter alone, a single
	// transient exception during a reload left the node flagged as broken forever while it was cooking
	// correctly, and TouchDesigner reports a node in an error state instead of its operator information -
	// which is one of the few things that can empty a middle-click popup with nothing else visibly wrong.
	// Set on a throw, cleared when an analysis completes and publishes.
	std::atomic<bool>		myPipelineFailing{ false };
	// HOW TO CHANGE: the pattern to preserve is "set on failure, cleared by the next success". Adding a
	// new way to fail means setting these two in the same place runJob() does, not inventing a third
	// latch - the whole value of myPipelineFailing is that exactly one thing clears it.

	// --- telemetry ---
	// Every number the UI reports about the node's own performance. The pattern throughout this block
	// is the same and is worth stating once: the pipeline owner thread WRITES, the Info callbacks
	// READ, and the read side must never take a lock or copy a string that a writer is holding. That
	// is why most of these are atomics and why the one piece of shared structured data (the status)
	// has its own memo on the read side - see statusSnapshot() and myStatusRead below.
	//
	// myDspUs: how long the last analysis took, in microseconds. Written by runJob after every
	// analysis, read by the DSP-time Info channel.
	std::atomic<double>	myDspUs{ 0.0 };
	// myOutputSampleRate: the exact spectrum rate read off the pipeline owner's own tables (not
	// recomputed from the parameters), so the reported number always describes the bins that were
	// actually produced. Written by runJob.
	std::atomic<double>	myOutputSampleRate{ 0.0 };    // exact spectrum rate read off the pipeline owner's tables
	// myStatusMutex: guards myStatusCopy only. Taken by the pipeline owner when it publishes a new
	// status, and by whichever thread is refreshing its memo in statusSnapshot(). It is deliberately
	// NOT taken by the cook in async mode, so a slow Info query can never delay a frame. If you add a
	// field under this mutex, add it to myStatusCopy's refresh in runJob() too.
	std::mutex			myStatusMutex;                // Info callbacks <-> pipeline owner; never taken by the cook in async mode
	// myStatusCopy: the pipeline's status struct as last published. Guarded by myStatusMutex. It moves
	// only on a plan rebuild, which is what makes the reader-side memo below effective.
	AnalysisPipeline::Status myStatusCopy;        // guarded by myStatusMutex; moves only on a plan rebuild
	// myStatusVersionSeen: the pipeline's status version at the last publish. Pipeline owner thread
	// only - it is what tells runJob() that the strings actually changed and are worth rebuilding.
	uint64_t			myStatusVersionSeen{ 0 };     // pipeline owner only
	// Reader-side memo of the above. TouchDesigner calls the info-chain callbacks one channel / one row at
	// a time - 21 Info CHOP channels plus 276 Info DAT rows plus the popup, per cook - and every one of
	// them asks for this struct. Taking the lock and copying two std::strings each time was ~850 heap
	// allocations per frame on the cook thread, which is both a real-time-path violation and the reason a
	// middle-click query could take a frame or two to answer. myStatusPubVersion is bumped by the pipeline
	// owner whenever it publishes; this thread compares it and re-copies only when it actually moved, so
	// the steady state is one atomic load and a returned reference.
	//
	// myStatusRead / myStatusReadVersion are written only by the thread that calls statusSnapshot(), which
	// is TouchDesigner's info thread - the info chain for a node is called serially within a cook.
	// myStatusPubVersion: bumped under myStatusMutex by the pipeline owner every time it writes
	// myStatusCopy. It is the whole signal the reader side gets - one atomic counter, no lock on the
	// reading thread's fast path.
	std::atomic<uint64_t> myStatusPubVersion{ 0 };  // bumped under myStatusMutex when myStatusCopy is written
	// myStatusRead: the read side's own copy, returned by reference by statusSnapshot(). TouchDesigner
	// asks for the status once per channel and once per row (~300 times a cook), so this exists purely
	// so those ~300 calls share one copy. Written only by whichever thread calls statusSnapshot().
	AnalysisPipeline::Status myStatusRead;
	// myStatusReadVersion: which published version myStatusRead was copied at. Compared against
	// myStatusPubVersion on each call; equal means the copy can be handed back untouched.
	uint64_t			myStatusReadVersion{ 0 };
	// False until the first copy is taken. Without it the pre-first-publish window (both counters still 0)
	// would compare equal and be read as "the memo is current", so the popup would report an empty engine
	// and zeros until the first plan was published.
	bool				myStatusReadValid{ false };
	// HOW TO CHANGE: these three only work together. If you add a fourth piece of state to the memo,
	// it must be refreshed in the same block in statusSnapshot() that sets myStatusReadValid, and the
	// initial-value case has to stay covered by myStatusReadValid rather than by a version compare.
	//
	// myLastCookUs: how long the last cook took, in microseconds. Cook thread only, reported in the Info
	// DAT.
	double				myLastCookUs{ 0.0 };
	// myPeakFrequencyHz / myPeakMagnitude: the loudest bin of the spectrum currently being output, and
	// its magnitude. Written by copyResultsToOutput from the published result, so they always describe
	// the frame the user is actually looking at rather than the newest one computed.
	float				myPeakFrequencyHz{ 0.0f };
	float				myPeakMagnitude{ 0.0f };
	// myCookDtMs / myCookRate: the frame time and the timeline rate of the last cook, taken from
	// TouchDesigner's own time info. Written by the cook, read by the Info callbacks. They are separate
	// because they answer different questions and only one of them is a constant: myCookRate is the
	// timeline FPS (a property of the project), while myCookDtMs is the real interval that elapsed
	// (which differs from 1/rate whenever a frame is dropped). outputBandwidth() uses the second;
	// anything that wants "how fast is this node supposed to run" wants the first.
	std::atomic<double>	myCookDtMs{ 1000.0 / 60.0 };  // last cook's frame time (outputBandwidth); cook writes, Info callbacks read
	std::atomic<double>	myCookRate{ 60.0 };           // me.time.rate (OP_TimeInfo::rate, the timeline FPS where this node lives); cook writes, Info callbacks read
	// myAsyncActive: whether the worker thread is actually running this cook - a copy of
	// myWorkerRunning taken after the Async parameter has been applied, so the Info output describes
	// what happened rather than what was requested.
	bool				myAsyncActive{ false };
	// myAnalysisChannels: how many channels the analysis ran on this cook. Reported, and also what the
	// Info output channel count and names are built from.
	int					myAnalysisChannels{ 0 };
	// myHoldFrames: how many cooks behind the displayed spectrum is (0 = this cook's own analysis).
	// Computed in copyResultsToOutput from the job/result sequence numbers; non-zero is normal with
	// Async on and is how a user sees the analysis falling behind.
	int					myHoldFrames{ 0 };
	// How many times TouchDesigner has actually entered each info callback. Nothing else in the plugin
	// can answer "is the popup empty because TD never called us, or because it called us and did not
	// render the text?", and the whole info chain runs inside a cook, so a node that is not cooking
	// leaves these at 0 with no other visible symptom. Reported three ways: the periodic log line, the
	// `info_callback_calls` Info DAT row, and (for the popup) the length below.
	//
	// They are one counter per callback because the chain is ordered: TouchDesigner asks for the CHOP
	// channel count, then the channels, then the DAT size, then the rows, then the popup. A counter
	// that is behind its predecessor says where the chain stopped.
	std::atomic<uint32_t> myInfoPopupCalls{ 0 };
	std::atomic<uint32_t> myInfoDatSizeCalls{ 0 };
	std::atomic<uint32_t> myInfoChopChansCalls{ 0 };
	// Length of the string last handed to TouchDesigner by getInfoPopupString. It is recorded because a
	// destination with a fixed capacity that is given more than it holds looks exactly like a popup that
	// was never filled, and every measurement of that failure so far has been blind to the one number
	// that would confirm it: a blank popup alongside a length that has grown past its previous values
	// says the text outgrew something, while a blank popup alongside an ordinary length says the text
	// was never the problem and the callback may not be entered at all.
	std::atomic<uint32_t> myInfoPopupLen{ 0 };
	// WHAT: the length, in characters, of the popup text last handed to TouchDesigner. Together with
	// the call counter above it splits "empty popup" into its two possible causes: called but handed
	// nothing, versus never called. HOW TO CHANGE: it must be set in getInfoPopupString() right where
	// the string is built - a stale value here is worse than no value, because the whole point is to
	// compare it against the blank popup on screen.
	//
	// Longest gap between two consecutive cooks, and the start of the previous cook. Cook thread only,
	// except the high-water mark, which the Info callbacks read. This is the one measurement that can
	// report a cook that did not happen: the node's whole information surface lives inside a cook, so a
	// stall blanks all of it at once and leaves nothing to explain itself. See executeImpl.
	//
	// myMaxCookGapMs is a high-water mark and is never reset by anything but a new node, so it answers
	// "the worst stall since this node was created". myLastCookStart is just the previous cook's clock
	// reading, used to measure the next gap; it starts at zero, which is how the first cook knows it
	// has nothing to compare against.
	std::atomic<double> myMaxCookGapMs{ 0.0 };
	std::chrono::steady_clock::time_point myLastCookStart{};
	// The plan-log rows for the Info DAT, frozen by getInfoDATSize() and consumed by
	// getInfoDATEntries(). It exists so the declared row count and the rows actually written describe the
	// same log: PlanLog::log() truncates the history by half at its cap, so reading myLog twice could see
	// two different lengths and leave TouchDesigner walking rows the plugin never filled. Cook thread only.
	std::vector<std::string> myInfoDatLog;
	// WHAT: the log's version number at the moment myInfoDatLog was copied. Equal to the log's current
	// version means the frozen copy above is still what getInfoDATSize() should declare. HOW TO CHANGE:
	// this is a pure optimisation - if it were removed and the copy re-taken every frame, the rows
	// would still be correct, just slower. Do not let it become the thing that decides the row count;
	// that is myInfoDatLog's job, and the note above says why.
	//
	// Which myLog version myInfoDatLog was copied from. The log only changes when the pipeline logs a plan
	// event, which is rare, so in the steady state this makes getInfoDATSize() an integer compare instead
	// of a full copy of up to 256 strings every frame. The freeze above is unaffected: this decides only
	// whether to re-take the copy, never how many rows the walk that follows sees.
	uint64_t myInfoDatLogVersion{ 0 };
	// Scratch for PlanLog::snapshotTail(), reused across cooks so the popup's log tail costs no allocation
	// once it has grown to size. Cook thread only.
	//
	// WHAT: a reusable string buffer, not data. Every copy of the log tail goes into this vector, whose
	// capacity survives between cooks, so the steady state allocates nothing for the popup. HOW TO
	// CHANGE: nothing reads this as state - if you ever treat its contents as meaningful before
	// snapshotTail() has refilled it, you will be reading the previous cook's log.
	std::vector<std::string> myPopupTail;
};

#endif // FFT_H
