// LayerPool.h - part of Plugin_FFT (TD_Custom_FFT). Original project code (README, "License / third party").
#ifndef LAYER_POOL_H
#define LAYER_POOL_H

/*
 * ===========================================================================
 *             LAYER POOL — splits one Multi-FFT job over a few threads (v2.13)
 * ===========================================================================
 * TD-free. Owned by the AnalysisPipeline; called only by the pipeline owner (the analysis worker).
 *
 * WHY A DEDICATED POOL AND NOT std::execution::par: the parallel STL runs on the Windows thread pool
 * at NORMAL priority, below the HIGHEST / MMCSS analysis worker that waits for it - a priority
 * inversion on a busy machine. These helpers take the worker's own scheduling (HIGHEST, opted out of
 * EcoQoS throttling), exactly like the analysis worker in AsyncAnalysis.cpp.
 *
 * WHY STATIC ASSIGNMENT: the tasks are FFTs whose cost is known in advance (~N log N), so the caller
 * hands each helper a fixed task list, balanced once per configuration (longest task first onto the
 * least-loaded thread). No work stealing, no shared counter, no per-job allocation: each helper has
 * its own mailbox (task list + posted/done generation numbers).
 *
 * THE HANDSHAKE, per helper: the caller writes the task list, stores posted = g (release) and wakes
 * the helper only if it went to sleep (the same Dekker pair as the worker's dormancy: helper stores
 * sleeping = true, full fence, re-checks posted; caller stores posted, full fence, loads sleeping).
 * The caller runs its own list, then waits for done == g. A helper spins ~50 us after a batch before
 * it sleeps, which catches the back-to-back batches of one job (the layer phase, then the per-channel
 * merge phase with All Channels) without a kernel wake.
 *
 * Results are bit-identical whatever the thread count: every task writes only its own buffers.
 * ===========================================================================
 */

#include "DSPModules.h"   // FFTDSP::WorkerSignal, DenormalGuard

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

class LayerPool {
public:
    using TaskFn = void (*)(void* ctx, int task) noexcept;

    LayerPool() = default;
    ~LayerPool() { resize(0); }
    LayerPool(const LayerPool&) = delete;
    LayerPool& operator=(const LayerPool&) = delete;

    // Number of helper threads (the caller is the extra one). Starting / joining threads happens only
    // here, i.e. when the Layer Threads parameter, Multi-FFT or Async changes - never per job.
    void resize(int helpers) {
        helpers = std::max(0, helpers);
        if (helpers == static_cast<int>(m_helpers.size())) return;
        for (auto& h : m_helpers) {
            h->stop.store(true, std::memory_order_release);
            h->signal.signal();
            if (h->thread.joinable()) h->thread.join();
        }
        m_helpers.clear();
        for (int i = 0; i < helpers; ++i) {
            m_helpers.push_back(std::make_unique<Helper>());
            Helper* h = m_helpers.back().get();
            h->thread = std::thread([h]() { helperLoop(*h); });
        }
    }
    int helpers() const noexcept { return static_cast<int>(m_helpers.size()); }

    // Runs bins[0] on the calling thread and bins[i] on helper i-1, and returns when all are done.
    // bins.size() may be smaller than helpers()+1 (the remaining helpers stay idle). A bin's vector must
    // not change until run() returns.
    void run(const std::vector<std::vector<int>>& bins, TaskFn fn, void* ctx) noexcept {
        const size_t nb = bins.size();
        if (nb == 0) return;
        const size_t posted = std::min(nb - 1, m_helpers.size());
        ++m_gen;
        for (size_t i = 0; i < posted; ++i) {
            Helper& h = *m_helpers[i];
            const std::vector<int>& b = bins[i + 1];
            if (b.empty()) continue;
            h.tasks = b.data();
            h.count = static_cast<int>(b.size());
            h.fn = fn;
            h.ctx = ctx;
            h.posted.store(m_gen, std::memory_order_release);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (h.sleeping.load(std::memory_order_seq_cst)) h.signal.signal();
        }
        for (int t : bins[0]) fn(ctx, t);
        for (size_t i = 0; i < posted; ++i) {
            if (bins[i + 1].empty()) continue;
            Helper& h = *m_helpers[i];
            unsigned spins = 0;
            while (h.done.load(std::memory_order_acquire) != m_gen) {
                if (++spins < 4096) _mm_pause(); else { std::this_thread::yield(); spins = 0; }
            }
        }
    }

    // Longest-processing-time-first assignment of tasks (with their costs) onto `threads` bins.
    static void balance(const std::vector<double>& cost, int threads, std::vector<std::vector<int>>& bins) {
        threads = std::max(1, threads);
        bins.assign(static_cast<size_t>(threads), {});
        std::vector<int> order(cost.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return cost[a] > cost[b]; });
        std::vector<double> load(static_cast<size_t>(threads), 0.0);
        for (int t : order) {
            size_t best = 0;
            for (size_t i = 1; i < load.size(); ++i) if (load[i] < load[best]) best = i;
            bins[best].push_back(t);
            load[best] += cost[t];
        }
        // The caller's bin (0) should be the heaviest: it starts first (no wake-up latency).
        size_t heavy = 0;
        for (size_t i = 1; i < load.size(); ++i) if (load[i] > load[heavy]) heavy = i;
        std::swap(bins[0], bins[heavy]);
        while (bins.size() > 1 && bins.back().empty()) bins.pop_back();
    }

private:
    struct alignas(64) Helper {
        std::thread thread;
        FFTDSP::WorkerSignal signal;
        alignas(64) std::atomic<uint64_t> posted{ 0 };
        alignas(64) std::atomic<uint64_t> done{ 0 };
        std::atomic<bool> sleeping{ false };
        std::atomic<bool> stop{ false };
        const int* tasks{ nullptr };
        int count{ 0 };
        TaskFn fn{ nullptr };
        void* ctx{ nullptr };
    };

    static void helperLoop(Helper& h) {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
            if (auto fn = reinterpret_cast<SetDescFn>(GetProcAddress(k32, "SetThreadDescription"))) fn(GetCurrentThread(), L"FFT Custom CHOP layer helper");
        }
#if defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)
        THREAD_POWER_THROTTLING_STATE st{};
        st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = 0;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof(st));
#endif
#endif
        FFTDSP::DenormalGuard ftz;                         // MXCSR is per thread, as on the worker
        uint64_t seen = 0;
        for (;;) {
            // Spin briefly (the next batch of the same job), then sleep until posted moves.
            const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
            uint64_t p = h.posted.load(std::memory_order_acquire);
            unsigned k = 0;
            while (p == seen && !h.stop.load(std::memory_order_relaxed)) {
                _mm_pause();
                if ((++k & 63) == 0 && std::chrono::steady_clock::now() > spinUntil) {
                    h.sleeping.store(true, std::memory_order_seq_cst);
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    p = h.posted.load(std::memory_order_seq_cst);
                    if (p == seen && !h.stop.load(std::memory_order_acquire)) h.signal.wait();
                    h.sleeping.store(false, std::memory_order_relaxed);
                }
                p = h.posted.load(std::memory_order_acquire);
            }
            if (h.stop.load(std::memory_order_acquire)) return;
            seen = p;
            for (int i = 0; i < h.count; ++i) h.fn(h.ctx, h.tasks[i]);
            h.done.store(seen, std::memory_order_release);
        }
    }

    std::vector<std::unique_ptr<Helper>> m_helpers;
    uint64_t m_gen{ 0 };
};

#endif // LAYER_POOL_H
