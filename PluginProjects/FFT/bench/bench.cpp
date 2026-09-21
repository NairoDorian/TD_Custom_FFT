// =============================================================================================
// fft_bench.exe - the FFT plugin's headless benchmark harness (bench/bench.cpp)
// =============================================================================================
//
// WHAT THIS FILE IS
//   Per-stage benchmark for the FFT plugin DSP pipeline (no TouchDesigner required).
//   It prints microseconds per stage per channel and the total per cook, so the real cost of
//   each parameter choice (FFT size, bins, scale, dB mode) can be measured instead of guessed.
//
// HOW IT FITS
//   The only project header it includes is "DSPModules.h", the TouchDesigner-independent DSP
//   header that the plugin (source/FFT.cpp), the headless unit tests (tests/dsp_tests.cpp) and
//   this bench all share - so the DSP measured here is the shipped DSP code, not a copy of it.
//   It is built as the CMake target fft_bench (PluginProjects/FFT/CMakeLists.txt); the other two
//   executables in this directory (fftw_threads_probe.cpp, fftw_version_probe.cpp) are separate
//   programs and share no code with this file.
//   It is run from a command line - by a person or by a script - and its only entry point is main();
//   nothing in the plugin calls into it.
//   Its numbers are quoted in README.md and CHANGELOG.md, so a measurement claim written here
//   must keep its exact value: the numbers are the record, the code only reproduces them.
//
// WHAT IT IS NOT
//   Not part of FFT.dll. Nothing in this file is compiled into the plugin and TouchDesigner never
//   loads or calls it: fft_bench.exe is a standalone console program. It can therefore only
//   measure what DSPModules.h exposes - everything the node does in FFT.cpp (parameters, the info
//   callbacks, the worker's wake policy) is out of reach here and is simulated, not reused. Where the
//   simulation deliberately mirrors the plugin (the worker's poll interval, its thread priority), the
//   comment on that line says so.
//
// EXACT COMMAND LINE FOR EACH MODE
//   build/bin/Release/fft_bench.exe --channels 8
//       default mode: per-stage table, plus the channel-loop fan-out when --channels >= 2
//   build/bin/Release/fft_bench.exe --channels 1 --db 0 --weight 0 --ball 0 --cook 300
//       --cook N: cook-thread cost distribution, Async on (worker) vs Async off (inline)
//   build/bin/Release/fft_bench.exe --info 2000
//       --info N: info-callback access cost (middle-click / Info CHOP / Info DAT path)
//   build/bin/Release/fft_bench.exe --backend mkl          (or --backend fftw3)
//       --backend: the FFT library A/B; names, not indices, resolved through the plugin's registry
//   The first three are the invocations README.md records. No machine is named in this file; the
//   machine that produced a number here is named in README.md's performance section, so a row
//   and a machine stay together there rather than being re-derived from this source.
//
// The flags those modes are spelled with:
//
//   build/bin/Release/fft_bench.exe [--channels N] [--fft N] [--win N] [--bins N] [--scale S]
//                                   [--iters N] [--planner auto|fast|measured|patient] [--db 0|1|2]
//                                   [--backend fftw3|mkl]
//                                   [--cook N]   simulate N cooks of the operator's cook thread
//                                                (async worker + sync) and report its cost distribution
//                                   [--info N]   measure N cooks of the info-callback access pattern
//                                                (the middle-click / Info CHOP / Info DAT path)
//
// --backend is the same choice the node's "FFT Backend" toggle makes, resolved through the same
// registry and the same runtime loading path, so a bench run is a fair A/B of the two libraries:
// everything else in the pipeline is identical, and the plan is built by whichever library is
// selected. The line it prints names the library, its path and its version, so a recorded number
// cannot be attributed to the wrong build.

#include "DSPModules.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execution>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace FFTDSP;
using clk = std::chrono::steady_clock;

// Everything the command line can set, and the value each mode runs with when the flag is absent.
// parse() fills this in; main() and the two mode functions read it. These defaults are the bench's
// own - they are not read from the node's parameters - so a run that does not pass a flag is not
// necessarily a run of the plugin's defaults.
struct Args {
    int channels = 2, fft = 32768, win = 3175, bins = 16384, scale = 0, iters = 200, db = 1, eq = 0, weight = 1, ball = 1;
    // channels: 2 is the stereo input the bench feeds and mono-mixes (see the cook mode's ingest).
    // fft = 32768 with bins = 16384 is the N and bin count the README performance tables are quoted
    // at; changing either default makes those tables describe a configuration nobody runs.
    // scale = 0 is Log scale (scale_code 0 in DSPModules.h; 5 is Linear, which is what makes the
    // warp identity and the axis equal the raw FFT grid - see the axis-rate print in main).
    // win = 3175, iters = 200 (value not derived in this file).
    int interp = 0;            // 0 linear, 1 cubic
    double fmax = 24000.0;     // Display Max Hz (limits the magnitude bins computed)
    int cook = 0;              // > 0: also run the cook-thread simulation for this many cooks
    int info = 0;              // > 0: also measure the info-callback access cost for this many cooks
    PlannerPolicy planner = PlannerPolicy::Auto;
    int backend = 0;           // registry index: 0 = FFTW3, 1 = oneMKL (see FftBackend.h)
};

// ===================== INFO-CALLBACK ACCESS COST (--info N) =====================
// ---------------------------------------------------------------------------------------------
// Info-callback access cost (--info N)
//
// The info chain is called *inside a cook*, so every microsecond it spends is added to the node's
// cook time - on the same thread that just ran the analysis. TouchDesigner drives it one item at a
// time: getNumInfoCHOPChans() then getInfoCHOPChan() per channel, getInfoDATSize() then
// getInfoDATEntries() per row, then getInfoPopupString(). Those callbacks all ask for the same
// status struct, and the popup also asks for the tail of the plan log. This measures the *access
// pattern*, not the DSP: the two loops below do identical work with the same data and differ only
// in whether each of the ~298 calls re-takes the copy, which is exactly the change being compared.
//
// The status struct is modelled locally rather than taken from AnalysisPipeline because the real one
// is reached through FFT.h, which needs the TouchDesigner SDK headers. The model mirrors the only
// property that costs anything here: two std::string members, so one copy is two allocations.
// ---------------------------------------------------------------------------------------------

// WHAT:  Times two versions of the info chain's data access - the "before" one that re-locks and
//        re-copies the status and the whole plan log on every item, and the "after" one that
//        memoizes the status behind a version stamp, skips the DAT copy when the log has not moved,
//        and copies only the three tail entries the popup renders - and prints us/cook for each.
// WHY:   The info chain runs inside a cook, so its cost is cook time. This is the number the popup's
//        redesign is justified by (the CHANGELOG records 54.6 -> 0.6 us/cook for --info 2000), and
//        the mode exists so that claim can be re-taken rather than re-argued.
// HOW TO CHANGE: The constants below mirror values that live in source/FFT.cpp (plan-log tail
//        length and per-line clip width) and the plugin's own call counts; changing one there
//        without changing it here makes this measurement describe a plugin that no longer exists.
// CALLED BY: main(), once, when --info is greater than 0.
static void infoPathBench(int cooks)
{
    // Stand-in for the plugin's status struct (see the note above). The string literals are shaped
    // like the real ones - a plan line naming the planner and its time, a backend line naming the
    // library and its codelets - because their *length* is the only thing this measurement reads.
    // 16385 = N/2+1 and 16384 = bins at the defaults above; 48000.0 is the axis rate the default
    // fmax of 24000 produces (2 * fmax). The 707.7 ms plan time the first string quotes is
    // (value not derived in this file).
    struct StatusLike {
        std::string plan = "FFTW3 (FFTW_MEASURE - 707.7 ms, N=32768)";
        std::string backend = "fftw3 3.3.11-avx2 (AVX2/FMA codelets)";
        bool planFailed{ false }, linearGrid{ true }, planUpgrading{ false };
        size_t fftSize{ 32768 }, capacity{ 32768 }, linearBins{ 16385 }, magnitudeBins{ 16384 };
        double axisRate{ 48000.0 }, axisBottom{ 0.0 };
        int outputBins{ 16384 };
    };

    // The measured call counts of one info chain: 21 Info CHOP channels, 276 Info DAT rows (20 fixed
    // + 256 plan-log rows), and the popup. Reported by the plugin's own counters, not assumed.
    const int kChopCalls = 21, kDatRows = 276;
    const int kStatusReads = kChopCalls + kDatRows + 1;
    // Mirrors FFT.cpp's kTailPlanLogLines, which the popup renders. It is file-local there (the DAT
    // rows and this bench are the only other things that read the log), so it is repeated here.
    const size_t kTail = 3;
    // ...and its kMaxTailLineChars, the width each of those lines is clipped to. The clip is mirrored
    // because it is the one part of the "after" path that the "before" path did not pay for at all: the
    // popup now truncates every rendered line rather than handing over a 240-character backend
    // description whole. It is a copy of at most 72 characters per line, so it must not be allowed to
    // hide inside the -99 % this prints - if it ever does, the number below will say so.
    // Keep in sync: source/FFT.cpp, kMaxTailLineChars (72). This is a copy, not a reference - the
    // real constant is file-local there and FFT.cpp pulls in the TouchDesigner SDK headers, so this
    // file cannot include it. Clipping a different width here than the popup uses would make this
    // mode's "after" number describe a plugin that no longer exists.
    const size_t kTailLineChars = 72;

    PlanLog log;
    for (size_t i = 0; i < kMaxPlanLogEntries; ++i)
        log.log("plan event " + std::to_string(i) + " - FFTW3 FFTW_MEASURE N=32768, 707.7 ms", false);

    std::mutex m;
    StatusLike published;
    size_t sink = 0;

    // --- before: every call re-locks and re-copies the status; the plan log is copied whole twice per
    //     cook, once for the DAT rows and once more for the popup's three-line tail.
    std::vector<std::string> datRows;                     // stands in for the myInfoDatLog member
    auto t0 = clk::now();
    for (int it = 0; it < cooks; ++it) {
        size_t acc = 0;
        for (int i = 0; i < kStatusReads; ++i) {
            std::lock_guard<std::mutex> lock(m);
            const StatusLike s = published;                              // by value: two allocations
            acc += s.plan.size() + s.backend.size();
        }
        datRows = log.snapshot();                                        // 256 string copies
        for (const auto& r : datRows) acc += r.size();
        const std::vector<std::string> whole = log.snapshot();            // 256 more, to use three
        for (size_t i = whole.size() > kTail ? whole.size() - kTail : 0; i < whole.size(); ++i)
            acc += whole[i].size();
        sink = acc;
    }
    const double us_old = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / cooks;

    // --- after: the status copy is memoized behind a version stamp, the DAT log copy is skipped when
    //     the log has not moved, and the popup asks for only the entries it renders.
    std::atomic<uint64_t> pubVersion{ 1 };
    StatusLike memo;
    uint64_t memoVersion = 0, datVersion = 0;
    const uint64_t logVer = log.version();
    std::vector<std::string> tail;
    auto t1 = clk::now();
    for (int it = 0; it < cooks; ++it) {
        size_t acc = 0;
        for (int i = 0; i < kStatusReads; ++i) {
            if (memoVersion != pubVersion.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(m);
                memo = published;
                memoVersion = pubVersion.load(std::memory_order_relaxed);
            }
            acc += memo.plan.size() + memo.backend.size();
        }
        if (logVer != datVersion) { datRows = log.snapshot(); datVersion = logVer; }
        for (const auto& r : datRows) acc += r.size();
        log.snapshotTail(kTail, tail);
        for (const auto& r : tail) acc += clipLine(r, kTailLineChars).size();
        sink = acc;
    }
    const double us_new = std::chrono::duration<double, std::micro>(clk::now() - t1).count() / cooks;

    std::printf("\ninfo-callback access cost (%d status reads + the %d-row log view, per cook, %d cooks):\n",
                kStatusReads, kDatRows, cooks);
    std::printf("  re-lock + re-copy every call  %9.1f us/cook\n", us_old);
    std::printf("  memoized + version-stamped    %9.1f us/cook   %+.0f%%\n", us_new, 100.0 * (us_new - us_old) / us_old);
    std::printf("  saved                         %9.1f us/cook   (%.2f%% of a 16.7 ms frame at 60 fps)\n",
                us_old - us_new, 100.0 * (us_old - us_new) / 16666.7);
    std::printf("  (sink %zu - keeps both loops honest)\n", sink);
}


// ===================== COOK-THREAD SIMULATION (--cook N) =====================
// ------------------------------------------------------------------------------------------
// Cook-thread simulation: exactly the work FFT::executeImpl does per cook with Async on
// (stereo mono-mix ingest -> FIFO -> job slot -> publish + wake -> acquire result -> memcpy to
// the output), with a worker thread running window -> FFT -> magnitude -> warp -> peak.
// Paced at ~60 fps so the worker is idle when the next cook arrives, like in TouchDesigner.
// ------------------------------------------------------------------------------------------

// WHAT:  Runs N simulated cooks twice - once with the analysis on a worker thread (Async on) and
//        once inline on the calling thread (Async off) - and prints mean/median/p99/max us per
//        cook for each, the async phase breakdown, and how long after publish the worker picked
//        the job up.
// WHY:   This is the cost the operator adds to TouchDesigner's cook thread each frame, which is the
//        number the Async design is justified by. TouchDesigner reports a node's cook time as one
//        total, so the distribution (p99, max), the sync-vs-async comparison and the phase split
//        come from here. Two things keep it honest and are deliberate: the cooks are paced (~60 fps,
//        so the worker is idle when the next cook arrives) and the caches are evicted between cooks.
//        Removing either makes the reported number better than the plugin's.
// HOW TO CHANGE: kPollMs and the worker's thread priority mirror the plugin's own worker
//        (FFT.cpp's workerLoop poll interval and nameAndBoostCurrentThread). If either changes
//        there, change it here too, or this stops simulating the shipped threading.
// CALLED BY: main(), when --cook (--cook N, N > 0) is present.
static void cookThreadBench(const Args& a, const FFTWEngine& engine, const AlignedVector& window,
                            const PerceptualWarping& warp, size_t n_mag, size_t pad_start)
{
    struct Job { std::vector<AlignedVector> windows; uint64_t seq{ 0 }; clk::time_point published; };
    struct Result { std::vector<AlignedVector> spectra; uint64_t seq{ 0 }; float peakHz{ 0.0f }; };
    const uint32_t kPollMs = 2;                                      // same policy as FFT::workerLoop
    const size_t ch = static_cast<size_t>(a.channels), win = static_cast<size_t>(a.win);
    const size_t N = static_cast<size_t>(a.fft), bins = warp.outputBins();
    const size_t block = 735;                                        // 44.1 kHz @ 60 fps (44100 / 60)

    TripleBuffer<Job> jobs;
    TripleBuffer<Result> results;
    WorkerSignal wake;
    std::atomic<bool> stop{ false };
    for (size_t s = 0; s < TripleBuffer<Job>::kSlots; ++s) {
        jobs.slot(s).windows.assign(ch, AlignedVector(win, 0.0f));
        results.slot(s).spectra.assign(ch, AlignedVector(bins, 0.0f));
    }
    // per-channel pipeline state (what AnalysisPipeline owns)
    std::vector<AlignedVector> frame(ch, AlignedVector(N, 0.0f)), mag(ch);
    std::vector<AlignedComplexVector> scratch(ch);
    std::vector<double> pickup_us;                                   // publish -> worker acquire latency
    pickup_us.reserve(static_cast<size_t>(a.cook) + 8);              // +8 slack (value not derived in this file)
    auto runJob = [&](const Job& j) {
        pickup_us.push_back(std::chrono::duration<double, std::micro>(clk::now() - j.published).count());
        Result& r = results.back();
        for (size_t c = 0; c < ch; ++c) {
            multiplyInto(j.windows[c].data(), window.data(), frame[c].data() + pad_start, win);
            engine.executeRFFT(frame[c], mag[c], scratch[c], n_mag);
            warp.applyWarp(mag[c], r.spectra[c]);
        }
        size_t idx = 0;
        findPeakWithIndex(r.spectra[0].data(), r.spectra[0].size(), idx);
        r.peakHz = static_cast<float>(warp.targetHz()[idx]);
        r.seq = j.seq;
        results.publish();
    };

    // cook-side state (what the FFT operator owns)
    std::vector<FIFOBuffer> fifo(ch);
    for (auto& f : fifo) f.resize(win);
    std::vector<float> inL(block), inR(block);
    // Input: two sines of different frequency, so L and R are not the same block and the mono mix has
    // something to mix. The amplitudes and the per-sample steps (0.3 / 0.2 / 0.1 / 0.37) are
    // (value not derived in this file): nothing here depends on the signal, only that it is not silent.
    for (size_t i = 0; i < block; ++i) { inL[i] = static_cast<float>(0.3 * std::sin(i * 0.1)); inR[i] = static_cast<float>(0.2 * std::sin(i * 0.37)); }
    AlignedVector mixed(block);
    std::vector<AlignedVector> output(ch, AlignedVector(bins, 0.0f));   // stands in for TouchDesigner's CHOP_Output buffers
    uint64_t seq = 0, dropped = 0, holds = 0;
    double us_ingest = 0, us_job = 0, us_wake = 0, us_copy = 0;      // async phase accumulators

    auto cook = [&](bool async) -> double {
        auto t0 = clk::now();
        DenormalGuard ftz;
        for (size_t c = 0; c < ch; ++c) {                            // ingest: mono mix of a stereo input
            std::memcpy(mixed.data(), inL.data(), block * sizeof(float));
            addInto(mixed.data(), inR.data(), mixed.data(), block);
            scaleInPlace(mixed.data(), block, 0.5f);                 // /2: the mix is (L+R)/2
            (void)blockIsSilent(mixed.data(), block);
            fifo[c].add(mixed.data(), block);
        }
        auto t1 = clk::now();
        Job& j = jobs.back();                                        // job snapshot
        j.seq = ++seq;
        for (size_t c = 0; c < ch; ++c) fifo[c].get(j.windows[c]);
        j.published = clk::now();
        if (jobs.publish()) ++dropped;
        auto t2 = clk::now();
        if (!async && jobs.acquire()) runJob(jobs.front());         // async: the hot worker polls; no wake-up call
        auto t3 = clk::now();
        results.acquire();                                           // output copy
        const Result& r = results.front();
        for (size_t c = 0; c < ch; ++c) std::memcpy(output[c].data(), r.spectra[c].data(), bins * sizeof(float));
        if (seq - r.seq > 1) ++holds;
        auto t4 = clk::now();
        if (async) {
            us_ingest += std::chrono::duration<double, std::micro>(t1 - t0).count();
            us_job    += std::chrono::duration<double, std::micro>(t2 - t1).count();
            us_wake   += std::chrono::duration<double, std::micro>(t3 - t2).count();
            us_copy   += std::chrono::duration<double, std::micro>(t4 - t3).count();
        }
        return std::chrono::duration<double, std::micro>(t4 - t0).count();
    };
    // Sorts t in place - callers hand over a vector they have finished with - then reports the mean,
    // the median and the p99 taken by index rather than interpolated between samples.
    auto report = [&](const char* label, std::vector<double>& t) {
        std::sort(t.begin(), t.end());
        double sum = 0; for (double v : t) sum += v;
        std::printf("  %-22s mean %6.2f us  median %6.2f us  p99 %6.2f us  max %7.2f us  (n=%zu)\n",
                    label, sum / t.size(), t[t.size() / 2], t[t.size() * 99 / 100], t.back(), t.size());
    };
    // Between cooks TouchDesigner's cook thread is busy with other operators: keep the core awake
    // and clocked up but stream through 8 MB so our working set is evicted from L1/L2 like it is in TD.
    std::vector<float> evict(2 * 1024 * 1024, 1.0f);                 // 2*1024*1024 floats * 4 B = 8 MB
    volatile float sink = 0.0f;
    auto pace = [&] {
        auto until = clk::now() + std::chrono::milliseconds(15);      // 15 ms of the 16.7 ms a 60 fps frame allows
        // i += 16 floats = 64 bytes: one touch per cache line, so the sweep reads every line it streams over.
        while (clk::now() < until) { float s = 0; for (size_t i = 0; i < evict.size(); i += 16) s += evict[i]; sink = s; }
    };

    std::thread worker([&] {
        // Match the plugin's worker (nameAndBoostCurrentThread in FFT.cpp) so the simulated
        // pipeline owner thread competes at the same priority level.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        DenormalGuard ftz;
        // wake is signaled only to stop this thread (after the timed loop), so during a run the
        // worker finds each job by polling - which is what "Async on" measures.
        for (;;) {
            if (stop.load()) return;
            if (jobs.acquire()) { runJob(jobs.front()); continue; }
            wake.waitFor(kPollMs);
        }
    });
    std::vector<double> t_async, t_sync;
    t_async.reserve(static_cast<size_t>(a.cook));
    t_sync.reserve(static_cast<size_t>(a.cook));
    for (int it = 0; it < 5; ++it) { cook(true); pace(); }         // warm-up (5 cooks: value not derived in this file)
    // The warm-up's phases are discarded so the first report is of warm caches and a live thread pool.
    dropped = 0; holds = 0; us_ingest = us_job = us_wake = us_copy = 0;
    for (int it = 0; it < a.cook; ++it) {
        pace();
        t_async.push_back(cook(true));
    }
    stop.store(true);
    wake.signal();
    worker.join();
    const uint64_t async_dropped = dropped, async_holds = holds;
    std::vector<double> async_pickup = pickup_us;
    for (int it = 0; it < a.cook; ++it) {
        pace();
        t_sync.push_back(cook(false));
    }
    std::printf("\ncook-thread cost per cook (%zu channel(s), %zu bins, N=%zu, stereo mono-mix ingest, ~60 fps pacing, caches evicted between cooks):\n", ch, bins, N);
    report("Async on (worker)", t_async);
    std::printf("  %-22s ingest %.2f  job snapshot+publish %.2f  (wake call %.2f)  acquire+output copy %.2f  (mean us)\n", "  async phases",
                us_ingest / a.cook, us_job / a.cook, us_wake / a.cook, us_copy / a.cook);
    report("Async off (inline)", t_sync);
    if (!async_pickup.empty()) {
        std::sort(async_pickup.begin(), async_pickup.end());
        std::printf("  async: worker picked the job up %.0f us (median) / %.0f us (max) after publish with a %u ms poll; "
                    "%llu job(s) dropped, %llu cook(s) held beyond the normal 1-frame latency\n",
                    async_pickup[async_pickup.size() / 2], async_pickup.back(), kPollMs,
                    static_cast<unsigned long long>(async_dropped), static_cast<unsigned long long>(async_holds));
    }
}

// ===================== ARGUMENT PARSING =====================
// WHAT:  Turns the command line into an Args. Every flag is a name/value pair, so the loop steps two
//        arguments at a time starting at argv[1] and stops one short of the end.
// WHY:   Pair-stepping is why a flag with a missing value is ignored rather than reported, and why an
//        unrecognized flag name is skipped in silence; --backend is the single exception and says so.
// HOW TO CHANGE: A new flag is a new `else if` in this chain - there is no table. Every flag has to
//        take a value, because the loop advances by two; a value-less switch would need this loop's
//        stepping changed, not just a branch added.
// CALLED BY: main(), once, before any measurement starts.
static Args parse(int argc, char** argv)
{
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--channels") a.channels = std::atoi(v);
        else if (k == "--fft") a.fft = std::atoi(v);
        else if (k == "--win") a.win = std::atoi(v);
        else if (k == "--bins") a.bins = std::atoi(v);
        else if (k == "--scale") a.scale = std::atoi(v);
        else if (k == "--iters") a.iters = std::atoi(v);
        else if (k == "--db") a.db = std::atoi(v);
        else if (k == "--eq") a.eq = std::atoi(v);
        else if (k == "--weight") a.weight = std::atoi(v);
        else if (k == "--ball") a.ball = std::atoi(v);
        else if (k == "--interp") a.interp = std::atoi(v);
        else if (k == "--fmax") a.fmax = std::atof(v);
        else if (k == "--cook") a.cook = std::atoi(v);
        else if (k == "--info") a.info = std::atoi(v);
        else if (k == "--planner") {
            std::string p = v;
            a.planner = (p == "fast") ? PlannerPolicy::Fast : (p == "measured") ? PlannerPolicy::Measured
                      : (p == "patient") ? PlannerPolicy::Patient : PlannerPolicy::Auto;
        }
        else if (k == "--backend") {
            // Names rather than indices, because a bench command line gets copied into a log or a
            // README and "1" says nothing a year later. Unknown names keep the default and say so.
            std::string b = v;
            if (b == "mkl" || b == "onemkl" || b == "intel") a.backend = 1;
            else if (b == "fftw3" || b == "fftw") a.backend = 0;
            else std::printf("unknown --backend '%s': using the default (fftw3)\n", b.c_str());
        }
    }
    return a;
}

// One row of the per-stage table in main: a label that must be a string literal (it is only ever
// printed) and the microseconds accumulated for that stage across every iteration and channel. The
// printed per-channel figure is this total divided by iters * channels.
struct Stage { const char* name; double us = 0.0; };

// ===================== ENTRY POINT =====================
// WHAT:  Runs the requested modes in a fixed order: always the per-stage timing table (plus the
//        channel-loop fan-out when there is more than one channel), then the cook-thread simulation
//        if --cook, then the info-callback measurement if --info.
// WHY:   The stage table comes first because its warm-up and its plan build are what make the later
//        modes measure a warm engine and tables that are already in cache.
// HOW TO CHANGE: Anything printed before the stage table is a header line, and both --cook and --info
//        append to the same stdout stream, so an added print lands in the middle of another mode's
//        output. Keep the engine prepared once: both optional modes reuse the plan built here.
// CALLED BY: the operating system, as the entry point of fft_bench.exe.
int main(int argc, char** argv)
{
    // Unbuffered stdout. When this process is piped (CI, a redirected log, rtk) the MSVC CRT block-
    // buffers stdout and abort()/an access violation does not flush it, so a crash loses *every* line
    // printed before it and the run looks like it produced no output at all rather than like it died.
    // The bench prints a few dozen lines outside the timing loop, so line-buffering costs nothing
    // measurable and buys a crash whose last line is the line it died on.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Args a = parse(argc, argv);
    const double sr = 44100.0;                                       // the input rate the whole run is modelled at
    const size_t N = static_cast<size_t>(a.fft), win = static_cast<size_t>(a.win), bins = static_cast<size_t>(a.bins);
    std::printf("FFT bench: channels=%d fft=%zu win=%zu bins=%zu scale=%d db=%d iters=%d (AVX2 %s)\n",
                a.channels, N, win, bins, a.scale, a.db, a.iters,
#if defined(__AVX2__)
                "on"
#else
                "off"
#endif
    );

    PlanLog log;
    FFTWEngine engine;
    const FftBackendInfo& want = backendById(a.backend);
    auto t0 = clk::now();
    engine.prepare(N, a.planner, &log, &want);
    std::printf("plan: %s (%.1f ms)\n", engine.getPlanStatus().c_str(),
                std::chrono::duration<double, std::milli>(clk::now() - t0).count());
    // Which library is actually doing the work, spelled out with its version and path: this is the
    // attribution line for every number that follows, and it is also where a requested-but-missing
    // oneMKL shows up as a fallback rather than as a silently mis-labelled result.
    std::printf("backend asked for: %s\n", want.display);
    std::printf("backend live:      %s\n", engine.backendReport().c_str());
    if (engine.upgradeInProgress()) {                                // Auto/Patient: wait for the background plan so the stage numbers use it
        std::printf("waiting for the background plan ...");
        std::fflush(stdout);
        while (!engine.pollBackgroundPlan()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::printf(" %s\n", engine.getPlanStatus().c_str());
    }

    AlignedVector window;
    // 0 = Kaiser (window_type in DSPModules.h), 15.0 = its kaiser_beta (value not derived in this
    // file); CoherentGain normalisation keeps mean(window) == 1.
    WindowGenerator::generateWindow(0, 15.0, win, window, WindowNorm::CoherentGain);
    PerceptualWarping warp;
    warp.setInterpolation(a.interp);
    const double fmax = a.fmax > sr / 2 ? sr / 2 : a.fmax;   // clamp to Nyquist: nothing above it to map
    // buildWarpTables(scale_code, fmax, n_out, nyquist, warp_blend, log_floor_hz, n_linear_bins):
    // sr / 2.0 is Nyquist, 0.963 is warp_blend (1.0 would be the pure perceptual grid, 0.0 the pure
    // linear one - so the grid is mostly perceptual with a little linear mixed in; value not derived
    // in this file), 20.0 Hz is log_floor_hz (the bottom of the axis), and N/2+1 is the linear R2C
    // bin count of an N-point real FFT (DC..Nyquist inclusive).
    warp.buildWarpTables(a.scale, fmax, bins, sr / 2.0, 0.963, 20.0, N / 2 + 1);
    const size_t n_mag = std::min(N / 2 + 1, warp.maxLinearIndex() + 1);
    std::printf("warp: %s interpolation, identity=%d, magnitude bins computed %zu of %zu\n",
                a.interp ? "cubic" : "linear", warp.isIdentity() ? 1 : 0, n_mag, N / 2 + 1);
    // Sample rate the CHOP reports for this spectrum: the axis is 0..fmax with the last bin on
    // Nyquist, so it is 2*fmax (see the output rate model in FFT.cpp). When the grid above comes
    // out identity - Linear scale, N/2+1 bins, fmax = nyquist, i.e. the raw FFT grid - that is
    // exactly the input rate. Hz per bin is the mean spacing; a warped grid is not uniform.
    // The band the axis covers, 2 x (top of the axis) - the CHOP reports bins x me.time.rate as
    // info->sampleRate instead, which is a throughput figure this bench has no frame rate for.
    std::printf("axis rate: %.1f Hz (axis 0..%.1f Hz over %zu bins, %.3f Hz per bin on average)\n",
                2.0 * fmax, fmax, bins, bins > 1 ? fmax / static_cast<double>(bins - 1) : 0.0);
    AlignedVector weighting;
    EqualLoudness::computeCurve(1, warp.targetHz(), weighting);      // 1 = A-weighting (0 off, 2 C, 3 ITU-R 468)

    // Everything one channel owns. These are per-channel on purpose: the plugin's fan-out (measured
    // below) is safe only because no two channels share any of this state.
    struct Ch {
        FIFOBuffer fifo; BiquadEQ eq; BallisticsFilter ball;
        AlignedVector captured, processed, frame, mag, warped, prev;
        AlignedComplexVector scratch;
    };
    std::vector<Ch> chans(static_cast<size_t>(a.channels));
    for (auto& c : chans) {
        c.fifo.resize(win);
        c.frame.assign(N, 0.0f);
        c.captured.resize(win);
    }
    std::vector<float> block(735);                                   // 44.1 kHz @ 60 fps (44100 / 60)
    // Input: a sine plus a quieter one at a much higher frequency (17x the first). The amplitudes and
    // the per-sample steps (0.3 / 0.1 / 0.1 / 1.7) are (value not derived in this file): the pipeline's
    // cost does not depend on the signal, only that it is not silence.
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<float>(0.3 * std::sin(i * 0.1) + 0.1 * std::sin(i * 1.7));
    // Center the window inside the padded frame when the window is shorter than the transform (the
    // padding is what the FFT sees before and after the data). & ~7 rounds the start down to a
    // multiple of 8 floats = 32 bytes, so frame.data() + pad_start stays AVX2-aligned.
    size_t pad_start = ((N > win) ? (N - win) / 2 : 0) & ~static_cast<size_t>(7);

    // ===================== PER-STAGE TIMING (default mode) =====================
    // The rows are the pipeline in order and each tick() call below closes the stage that just ran, so
    // this list and the order of the tick() calls have to stay in step: a row with no tick prints 0.00,
    // and time between two ticks is charged to the earlier stage. One thing worth knowing when reading
    // the table: with --eq 1 the filter runs at ingest, before the first tick, so its cost lands in the
    // "fifo" row and the "eq" row measures the legacy --eq 2 path. Everything above this line is per
    // run, not per cook, and is not timed anywhere.
    Stage stages[] = { {"fifo"}, {"eq"}, {"window"}, {"fft+mag"}, {"warp"}, {"weighting"}, {"dB"}, {"ballistics"}, {"peak"} };
    // Adds the time since t to s and moves t forward, so consecutive ticks partition the loop exactly:
    // no interval is counted twice, and everything between two ticks lands in the earlier stage.
    auto tick = [](clk::time_point& t, Stage& s) {
        auto n = clk::now();
        s.us += std::chrono::duration<double, std::micro>(n - t).count();
        t = n;
    };

    // warm-up (5 iterations: value not derived in this file) so the timed loop does not report the
    // first-touch cost of the buffers and tables
    for (int it = 0; it < 5; ++it) {
        for (auto& c : chans) {
            c.fifo.add(block.data(), block.size());
            c.fifo.get(c.captured);
            multiplyInto(c.captured.data(), window.data(), c.frame.data() + pad_start, win);
            engine.executeRFFT(c.frame, c.mag, c.scratch);
            warp.applyWarp(c.mag, c.warped);
        }
    }

    auto total0 = clk::now();
    // eq is the A/B this mode exists to make: 0 = EQ off (the default), 1 = filter only the new block
    // at ingest (current), 2 = re-filter the whole analysis window every frame (legacy). The two paths
    // are only comparable because both are driven with the same filter design below.
    for (int it = 0; it < a.iters; ++it) {
        for (auto& c : chans) {
            auto t = clk::now();
            if (a.eq == 1) {
                // current: filter only the new block at ingest (stateful IIR, time order)
                // updateAndCheckActive(gain_db, high_cutoff_hz, low_gain_db, low_cutoff_hz, q, amount):
                // a 6 dB high shelf at 1 kHz, the low shelf off (0 dB, so its 200 Hz corner is unused),
                // q = 0.707 = 1/sqrt(2), fully applied.
                bool has = c.eq.updateAndCheckActive(6.0, 1000.0, 0.0, 200.0, 0.707, 1.0);
                if (c.processed.size() < block.size()) c.processed.resize(block.size());
                std::memcpy(c.processed.data(), block.data(), block.size() * sizeof(float));
                if (has) c.eq.processBlockInPlace(c.processed.data(), block.size(), 1.0);
                c.fifo.add(c.processed.data(), block.size());
            } else {
                c.fifo.add(block.data(), block.size());
            }
            c.fifo.get(c.captured);
            tick(t, stages[0]);
            const float* src = c.captured.data();
            if (a.eq == 2) {
                // legacy: re-filter the whole analysis window every frame
                bool has = c.eq.updateAndCheckActive(6.0, 1000.0, 0.0, 200.0, 0.707, 1.0);   // plugin defaults: 6 dB high shelf
                if (has) { c.eq.processAudio(c.captured, 1.0, c.processed); src = c.processed.data(); }
            }
            tick(t, stages[1]);
            multiplyInto(src, window.data(), c.frame.data() + pad_start, win);
            tick(t, stages[2]);
            engine.executeRFFT(c.frame, c.mag, c.scratch, n_mag);
            tick(t, stages[3]);
            warp.applyWarp(c.mag, c.warped);
            tick(t, stages[4]);
            if (a.weight) multiplyInPlace(c.warped.data(), weighting.data(), c.warped.size());
            tick(t, stages[5]);
            if (a.db) {
                // convertToDB(mode, top_db, inv_ref): 80.0 is the dB range (the bottom of the display),
                // and the reference is the frame peak, so the loudest bin reads 0 dB.
                float pk = peakMagnitude(c.warped.data(), c.warped.size());
                DecibelConverter::convertToDB(a.db, 80.0, 1.0f / (pk > 0 ? pk : 1.0f), c.warped);
            }
            tick(t, stages[6]);
            // apply(attack, release, ...): per-frame coefficients in [0, 0.99] (not milliseconds, and
            // 0 would follow instantly), per BallisticsFilter in DSPModules.h.
            if (a.ball) c.ball.apply(0.3f, 0.6f, c.warped, c.prev);
            tick(t, stages[7]);
            size_t idx; findPeakWithIndex(c.warped.data(), c.warped.size(), idx);
            tick(t, stages[8]);
        }
    }
    double total_us = std::chrono::duration<double, std::micro>(clk::now() - total0).count();
    double per_cook = total_us / a.iters;
    double per_ch = per_cook / a.channels;

    std::printf("\n%-12s %12s %10s\n", "stage", "us/channel", "share");
    double sum = 0;
    for (auto& s : stages) sum += s.us;
    for (auto& s : stages) {
        double us = s.us / (a.iters * a.channels);
        std::printf("%-12s %12.2f %9.1f%%\n", s.name, us, 100.0 * s.us / sum);
    }
    std::printf("%-12s %12.2f\n", "TOTAL", per_ch);
    // 16666.7 us = 1/60 s and 8333.3 us = 1/120 s: the frame budgets the percentages are against.
    std::printf("\nper cook (%d channels): %.1f us  = %.2f%% of a 60 fps frame, %.2f%% of a 120 fps frame\n",
                a.channels, per_cook, 100.0 * per_cook / 16666.7, 100.0 * per_cook / 8333.3);

    // ===================== CHANNEL-LOOP FAN-OUT =====================
    // ----------------------------------------------------------------------------------------
    // Channel-loop fan-out - the plugin's actual threading design: with more than one analysis
    // channel it runs exactly this loop under std::execution::par, unconditionally (there is no
    // parameter; a single-channel cook has nothing to fan out). Same per-channel pipeline as the
    // stage table above, minus the per-stage clocks, so the two totals line up. Bulk work (the
    // FFT) dominates, so measuring the fan-out here measures what the plugin does.
    // ----------------------------------------------------------------------------------------
    // The comparison is made fair in two steps, and both matter: the parallel run starts from a copy
    // of the state the serial run ended with (identical DSP history, identical inputs), and the two
    // outputs are then compared bin for bin - a speedup is not accepted if the parallel path computed
    // something else. The warm calls exist so neither measurement pays for the thread pool starting up.
    if (a.channels >= 2) {
        // Same per-channel pipeline as the timed loop above, with the tick() calls removed: if a stage
        // is added, reordered or re-parameterised there, it has to change here too, or the fan-out
        // compares two pipelines that no longer match the one the stage table measured.
        auto pipeline1 = [&](size_t i) {
            Ch& c = chans[i];
            c.fifo.add(block.data(), block.size());
            c.fifo.get(c.captured);
            multiplyInto(c.captured.data(), window.data(), c.frame.data() + pad_start, win);
            engine.executeRFFT(c.frame, c.mag, c.scratch, n_mag);
            warp.applyWarp(c.mag, c.warped);
            if (a.weight) multiplyInPlace(c.warped.data(), weighting.data(), c.warped.size());
            if (a.db) {
                float pk = peakMagnitude(c.warped.data(), c.warped.size());
                DecibelConverter::convertToDB(a.db, 80.0, 1.0f / (pk > 0 ? pk : 1.0f), c.warped);
            }
            if (a.ball) c.ball.apply(0.3f, 0.6f, c.warped, c.prev);
            size_t idx; findPeakWithIndex(c.warped.data(), c.warped.size(), idx);
        };
        std::vector<size_t> order(chans.size());
        std::iota(order.begin(), order.end(), 0);
        auto runAll = [&](bool parallel) {
            auto t = clk::now();
            if (parallel) std::for_each(std::execution::par, order.begin(), order.end(), pipeline1);
            else          std::for_each(order.begin(), order.end(), pipeline1);
            return std::chrono::duration<double, std::micro>(clk::now() - t).count();
        };
        runAll(false); runAll(true);                       // warm both paths (and the TBB pool)
        const std::vector<Ch> fresh = chans;               // per-channel DSP state, to restart from
        double us_serial = 0.0, us_par = 0.0;
        for (int it = 0; it < a.iters; ++it) us_serial += runAll(false);
        std::vector<AlignedVector> serial_out(chans.size());
        for (size_t i = 0; i < chans.size(); ++i) serial_out[i] = chans[i].warped;
        chans = fresh;                                     // identical state, identical inputs
        for (int it = 0; it < a.iters; ++it) us_par += runAll(true);
        double worst = 0.0;
        size_t worst_i = 0, worst_b = 0;
        for (size_t i = 0; i < chans.size(); ++i) {
            const AlignedVector& b = chans[i].warped;
            if (b.size() != serial_out[i].size()) { worst = 1e9; break; }   // 1e9: "shapes differ", not a measured delta
            for (size_t k = 0; k < b.size(); ++k) {
                const double d = std::fabs(static_cast<double>(b[k]) - serial_out[i][k]);
                if (d > worst) { worst = d; worst_i = i; worst_b = k; }
            }
        }
        const double s = us_serial / a.iters, p = us_par / a.iters;
        std::printf("\nchannel-loop fan-out (unconditional std::execution::par, %d channels, %d iters):\n", a.channels, a.iters);
        std::printf("  serial             %8.1f us/cook\n", s);
        std::printf("  std::execution::par %8.1f us/cook   %+.0f%%\n", p, 100.0 * (p - s) / s);
        std::printf("  outputs identical: %s (max |delta| %.3g at channel %zu bin %zu)\n",
                    worst == 0.0 ? "yes, bin for bin" : "NO - the parallel path is not equivalent",
                    worst, worst_i, worst_b);
    }

    // ===================== OPTIONAL MODES =====================
    // Both run last and print after everything above, so their output is never mixed into the stage
    // table they follow. They reuse the plan and the warp tables built at the top of main: neither
    // builds its own engine, so neither shows a plan build in its numbers.
    if (a.cook > 0) cookThreadBench(a, engine, window, warp, n_mag, pad_start);
    if (a.info > 0) infoPathBench(a.info);
    return 0;
}
