// AsyncAnalysis.cpp - part of Plugin_FFT (TD_Custom_FFT). See AsyncAnalysis.h for the contract.

#include "AsyncAnalysis.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

using clk = std::chrono::steady_clock;

inline int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count(); }

#ifdef _WIN32
// Worker thread scheduling. Called once, as the worker thread's first action.
//   Highest : THREAD_PRIORITY_HIGHEST - above TouchDesigner's normal threads (including the cook that
//             hands it work), so a busy machine cannot push a ~50 us burst into the next frame. Not
//             TIME_CRITICAL: that can starve the audio and UI threads for no gain on a short burst.
//   Mmcss   : registered with the Multimedia Class Scheduler as "Pro Audio" - scheduled ahead of normal
//             threads by the OS service that audio engines use, with a CPU reservation that keeps a
//             runaway from starving the system. avrt.dll is resolved at run time (no link dependency).
// Either way the thread is opted out of EcoQoS execution-speed throttling, which Windows 11 applies to
// threads of background/occluded processes and which stretches both the burst and the 2 ms poll.
struct ThreadScheduling {
    HANDLE mmcss{ nullptr };
    using SetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    using RevertFn = BOOL(WINAPI*)(HANDLE);
    RevertFn revert{ nullptr };

    void apply(Parameters::WorkerPriority prio, const wchar_t* name) {
        using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
            if (auto fn = reinterpret_cast<SetDescFn>(GetProcAddress(k32, "SetThreadDescription"))) fn(GetCurrentThread(), name);
        }
#if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)
        THREAD_POWER_THROTTLING_STATE st{};
        st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = 0;                                   // 0 = explicitly NOT throttled
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof(st));
#endif
        if (prio == Parameters::WorkerPriority::Mmcss) {
            if (HMODULE avrt = LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
                auto set = reinterpret_cast<SetFn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"));
                revert = reinterpret_cast<RevertFn>(GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"));
                DWORD task = 0;
                if (set) mmcss = set(L"Pro Audio", &task);
            }
            if (mmcss) return;                              // MMCSS owns the priority now
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    ~ThreadScheduling() { if (mmcss && revert) revert(mmcss); }
};
#endif

} // namespace

AsyncAnalysis::AsyncAnalysis(AnalysisPipeline& pipeline, FFTDSP::PlanLog& log)
    : myPipeline(pipeline), myLog(log)
{
}

AsyncAnalysis::~AsyncAnalysis()
{
    stopWorker();
}

void AsyncAnalysis::configure(bool async, Parameters::WorkerWake wake, Parameters::WorkerPriority priority)
{
    myWake.store(wake, std::memory_order_relaxed);         // read by publish() and by the worker
    if (async && myRunning && priority != myPriority) stopWorker();   // priority is applied at thread start
    myPriority = priority;
    if (async != myRunning) {
        if (async) startWorker(); else stopWorker();
    }
}

void AsyncAnalysis::publish()
{
    myJobs.back().publishedNs = nowNs();
    // A publish that overwrites a job the worker never took is a dropped analysis frame: normal under
    // load, reported as jobs_dropped, never an error.
    if (myJobs.publish()) myJobsDropped.fetch_add(1, std::memory_order_relaxed);
    if (myRunning) {
        if (myWake.load(std::memory_order_relaxed) == Parameters::WorkerWake::Signal) {
            mySignal.signal();                              // every cook: ~5-25 us here, pickup ~0.02 ms
            return;
        }
        // Poll: the hot worker finds the job itself. Only a dormant one needs the (kernel) wake-up; the
        // fence + load pair with the worker's store + fence (see the handshake note in the header).
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (myDormant.load(std::memory_order_seq_cst)) mySignal.signal();
    } else if (myJobs.acquire()) {
        runJob(myJobs.front());                             // Async off: inline, zero-frame latency
    }
}

void AsyncAnalysis::runJob(const AnalysisJob& job)
{
    AnalysisResult& res = myResults.back();
    try {
        myPipeline.process(job, res);
    } catch (...) {
        // Not published: the previous result stays visible. Lifetime count for the Info DAT, live flag
        // for the error string (set on failure, cleared by the next success - nothing else clears it).
        myPipelineErrors.fetch_add(1, std::memory_order_relaxed);
        myPipelineFailing.store(true, std::memory_order_relaxed);
        myLog.log("[FFT Plugin] [pipeline] analysis threw an exception; previous spectrum retained");
        return;
    }
    myPlanFailed.store(myPipeline.planFailed(), std::memory_order_relaxed);
    res.seq = job.seq;
    myResults.publish();
    myPipelineFailing.store(false, std::memory_order_relaxed);
    myDspUs.store(myPipeline.lastUs(), std::memory_order_relaxed);
    myAxisRate.store(myPipeline.outputSampleRate(), std::memory_order_relaxed);

    const uint64_t ver = myPipeline.statusVersion();
    if (ver != myStatusVersionSeen) {                       // strings are built only when plan/tables changed
        std::lock_guard<std::mutex> lock(myStatusMutex);
        myStatusCopy = myPipeline.status();
        myStatusVersionSeen = ver;
        myStatusPubVersion.fetch_add(1, std::memory_order_release);
    }
}

bool AsyncAnalysis::copyStatusIfNewer(uint64_t& seenVersion, bool& valid, AnalysisPipeline::Status& out)
{
    if (valid && seenVersion == myStatusPubVersion.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(myStatusMutex);
    out = myStatusCopy;
    seenVersion = myStatusPubVersion.load(std::memory_order_relaxed);
    valid = true;
    return true;
}

void AsyncAnalysis::startWorker()
{
    if (myRunning) return;
    myStop.store(false, std::memory_order_release);
    myDormant.store(false, std::memory_order_release);
    myRunning = true;
    myWorker = std::thread([this]() { workerLoop(); });
}

void AsyncAnalysis::stopWorker()
{
    if (!myRunning) return;
    myStop.store(true, std::memory_order_release);
    mySignal.signal();                                      // a dormant worker would never see the flag
    if (myWorker.joinable()) myWorker.join();
    myRunning = false;
}

void AsyncAnalysis::recordPickup(double us, double frameMs) noexcept
{
    myPickupRing[myPickupCount % kPickupWindow] = static_cast<float>(us);
    ++myPickupCount;
    if (frameMs > 0.0 && us > frameMs * 1000.0) myPickupLate.fetch_add(1, std::memory_order_relaxed);
    if (myPickupCount % 16 != 0) return;                    // summarise every 16 jobs
    const size_t n = std::min(myPickupCount, kPickupWindow);
    std::array<float, kPickupWindow> tmp;
    std::copy(myPickupRing.begin(), myPickupRing.begin() + static_cast<std::ptrdiff_t>(n), tmp.begin());
    auto at = [&](double q) {
        const size_t k = std::min(n - 1, static_cast<size_t>(q * static_cast<double>(n - 1) + 0.5));
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(k), tmp.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<double>(tmp[k]);
    };
    myPickupP50.store(at(0.50), std::memory_order_relaxed);
    myPickupP99.store(at(0.99), std::memory_order_relaxed);
    myPickupMax.store(static_cast<double>(*std::max_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n))), std::memory_order_relaxed);
}

AsyncAnalysis::Pickup AsyncAnalysis::pickup() const noexcept
{
    Pickup p;
    p.p50Us = myPickupP50.load(std::memory_order_relaxed);
    p.p99Us = myPickupP99.load(std::memory_order_relaxed);
    p.maxUs = myPickupMax.load(std::memory_order_relaxed);
    p.late = myPickupLate.load(std::memory_order_relaxed);
    return p;
}

// The worker's whole body: HOT (a job arrived recently: take jobs as they come - by polling every
// kWorkerPollMs with Wake = Poll, or woken per job with Wake = Signal) or DORMANT (no job for
// kWorkerDormantAfterMs: block until the cook signals once - the idle node costs nothing).
void AsyncAnalysis::workerLoop()
{
#ifdef _WIN32
    ThreadScheduling sched;
    sched.apply(myPriority, L"FFT Custom CHOP analysis");
#endif
    FFTDSP::DenormalGuard ftz;                              // MXCSR is per thread
    auto lastJob = clk::now();
    for (;;) {
        if (myStop.load(std::memory_order_acquire)) return;
        if (myJobs.acquire()) {                             // latest job wins
            const AnalysisJob& job = myJobs.front();
            if (job.publishedNs) recordPickup(static_cast<double>(nowNs() - job.publishedNs) / 1000.0, job.dtMs);
            runJob(job);
            lastJob = clk::now();
            myDormant.store(false, std::memory_order_relaxed);
            continue;
        }
        if (!myDormant.load(std::memory_order_relaxed)) {
            if (std::chrono::duration<double, std::milli>(clk::now() - lastJob).count() > kWorkerDormantAfterMs) {
                myDormant.store(true, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                continue;                                   // re-check the slot before sleeping
            }
            // Signal policy: sleep until the cook's per-job wake-up (bounded, so a stop request and the
            // dormancy clock are still noticed); Poll policy: the 2 ms high-resolution timer.
            mySignal.waitFor(myWake.load(std::memory_order_relaxed) == Parameters::WorkerWake::Signal ? 50u : kWorkerPollMs);
        } else {
            mySignal.wait();                                // dormant: until the cook signals
        }
    }
}
