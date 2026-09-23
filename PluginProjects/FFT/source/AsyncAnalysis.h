#ifndef ASYNC_ANALYSIS_H
#define ASYNC_ANALYSIS_H

/*
 * ===========================================================================
 *             ASYNC ANALYSIS — the cook <-> worker handoff, TD-free
 * ===========================================================================
 * AsyncAnalysis.h / AsyncAnalysis.cpp
 *
 * Everything between "the cook has a window of samples" and "the cook has a spectrum to copy out":
 * the two wait-free triple buffers, the analysis worker thread, its wake policy and dormancy
 * handshake, failure escalation and the cross-thread telemetry. It used to live inside FFT.cpp, which
 * made it untestable (FFT.cpp needs the TouchDesigner SDK) and forced the bench to simulate a copy of
 * it. Now the plugin, tests/dsp_tests.cpp and bench/bench.cpp all drive this one implementation.
 *
 * THREADING CONTRACT
 *   cook thread   : configure(), jobSlot(), publish(), acquireResult(), result()
 *   worker thread : workerLoop() -> runJob() -> AnalysisPipeline::process()   (Async on)
 *   cook thread   : runJob() inline from publish()                            (Async off)
 *   any thread    : the telemetry getters and copyStatusIfNewer()
 * The cook side never takes a lock or makes a kernel call in steady state, with two conditional
 * exceptions: one wake-up when the worker had gone dormant, and one per cook with Wake = Signal.
 *
 * DORMANCY HANDSHAKE (Dekker): the worker stores dormant=true, full fence, then re-checks the job slot
 * before sleeping; the cook publishes, full fence, then loads dormant and signals if set. One side
 * always observes the other's store, so a job published at that instant is never left unprocessed.
 * ===========================================================================
 */

#include "AnalysisPipeline.h"

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

class AsyncAnalysis {
public:
    // Pickup latency: publish -> worker acquire. Published every kPickupWindow jobs from the last
    // kPickupWindow samples (no allocation: a fixed ring and std::nth_element on a fixed copy).
    struct Pickup { double p50Us{ 0.0 }, p99Us{ 0.0 }, maxUs{ 0.0 }; uint64_t late{ 0 }; };

    AsyncAnalysis(AnalysisPipeline& pipeline, FFTDSP::PlanLog& log);
    ~AsyncAnalysis();
    AsyncAnalysis(const AsyncAnalysis&) = delete;
    AsyncAnalysis& operator=(const AsyncAnalysis&) = delete;

    // ---- cook thread ------------------------------------------------------------------------------
    // Brings the worker in line with the requested mode. A no-op when nothing changed (every cook);
    // starting/stopping/restarting the thread is the one operation that may cost a frame.
    void configure(bool async, Parameters::WorkerWake wake, Parameters::WorkerPriority priority);
    bool async() const noexcept { return myRunning; }
    // The slot the cook fills for this job. Valid until publish().
    AnalysisJob& jobSlot() noexcept { return myJobs.back(); }
    // Publishes the filled slot. Async: hands it to the worker (wake-up only if it is dormant or the
    // policy is Signal). Sync: runs the analysis right here, before returning.
    void publish();
    // Takes the newest finished result if there is one (one atomic exchange); result() is then it.
    bool acquireResult() noexcept { return myResults.acquire(); }
    const AnalysisResult& result() const noexcept { return myResults.front(); }

    // ---- telemetry (any thread) -------------------------------------------------------------------
    uint64_t jobsDropped() const noexcept { return myJobsDropped.load(std::memory_order_relaxed); }
    uint64_t pipelineErrors() const noexcept { return myPipelineErrors.load(std::memory_order_relaxed); }
    bool pipelineFailing() const noexcept { return myPipelineFailing.load(std::memory_order_relaxed); }
    bool planFailed() const noexcept { return myPlanFailed.load(std::memory_order_relaxed); }
    double dspUs() const noexcept { return myDspUs.load(std::memory_order_relaxed); }
    double axisRate() const noexcept { return myAxisRate.load(std::memory_order_relaxed); }
    bool parallelActive() const noexcept { return myPipeline.parallelActive(); }
    const FFTDSP::FftBackendInfo& backendInfo() const noexcept { return myPipeline.backendInfo(); }
    Pickup pickup() const noexcept;
    // Copies the pipeline status into `out` only if it changed since `seenVersion` (then updates it).
    // Steady state: one acquire load, no lock, no allocation.
    bool copyStatusIfNewer(uint64_t& seenVersion, bool& valid, AnalysisPipeline::Status& out);

    static constexpr uint32_t kWorkerPollMs = 2;         // job pickup latency while hot, Wake = Poll
    static constexpr double   kWorkerDormantAfterMs = 500.0;
    static constexpr size_t   kPickupWindow = 128;

private:
    void runJob(const AnalysisJob& job);                  // pipeline owner: worker, or cook when sync
    void startWorker();
    void stopWorker();
    void workerLoop();
    void recordPickup(double us, double frameMs) noexcept;   // worker only

    AnalysisPipeline& myPipeline;
    FFTDSP::PlanLog& myLog;

    FFTDSP::TripleBuffer<AnalysisJob>    myJobs;           // cook -> pipeline owner
    FFTDSP::TripleBuffer<AnalysisResult> myResults;        // pipeline owner -> cook

    std::thread myWorker;
    bool myRunning{ false };                               // cook thread only
    std::atomic<Parameters::WorkerWake> myWake{ Parameters::WorkerWake::Poll };   // cook writes, worker reads
    Parameters::WorkerPriority myPriority{ Parameters::WorkerPriority::Highest };
    std::atomic<bool> myStop{ false };
    std::atomic<bool> myDormant{ false };
    FFTDSP::WorkerSignal mySignal;

    std::atomic<uint64_t> myJobsDropped{ 0 };
    std::atomic<uint64_t> myPipelineErrors{ 0 };
    std::atomic<bool>     myPipelineFailing{ false };
    std::atomic<bool>     myPlanFailed{ false };
    std::atomic<double>   myDspUs{ 0.0 };
    std::atomic<double>   myAxisRate{ 0.0 };

    // status publication (pipeline owner writes under the mutex, readers memoize by version)
    std::mutex myStatusMutex;
    AnalysisPipeline::Status myStatusCopy;
    uint64_t myStatusVersionSeen{ 0 };                     // pipeline owner only
    std::atomic<uint64_t> myStatusPubVersion{ 0 };

    // pickup statistics (worker writes the ring; atomics are the published summary)
    std::array<float, kPickupWindow> myPickupRing{};
    size_t myPickupCount{ 0 };
    std::atomic<double> myPickupP50{ 0.0 }, myPickupP99{ 0.0 }, myPickupMax{ 0.0 };
    std::atomic<uint64_t> myPickupLate{ 0 };
};

#endif // ASYNC_ANALYSIS_H
