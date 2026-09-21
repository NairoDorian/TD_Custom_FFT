// Per-stage benchmark for the FFT plugin DSP pipeline (no TouchDesigner required).
//
//   build/bin/Release/fft_bench.exe [--channels N] [--fft N] [--win N] [--bins N] [--scale S]
//                                   [--iters N] [--planner auto|fast|measured|patient] [--db 0|1|2]
//                                   [--backend fftw3|mkl]
//                                   [--cook N]   simulate N cooks of the operator's cook thread
//                                                (async worker + sync) and report its cost distribution
//
// Prints microseconds per stage per channel and the total per cook, so the real cost of
// each parameter choice (FFT size, bins, scale, dB mode) can be measured instead of guessed.
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

struct Args {
    int channels = 2, fft = 32768, win = 3175, bins = 16384, scale = 0, iters = 200, db = 1, eq = 0, weight = 1, ball = 1;
    int interp = 0;            // 0 linear, 1 cubic
    double fmax = 24000.0;     // Display Max Hz (limits the magnitude bins computed)
    int cook = 0;              // > 0: also run the cook-thread simulation for this many cooks
    PlannerPolicy planner = PlannerPolicy::Auto;
    int backend = 0;           // registry index: 0 = FFTW3, 1 = oneMKL (see FftBackend.h)
};

// ------------------------------------------------------------------------------------------
// Cook-thread simulation: exactly the work FFT::executeImpl does per cook with Async on
// (stereo mono-mix ingest -> FIFO -> job slot -> publish + wake -> acquire result -> memcpy to
// the output), with a worker thread running window -> FFT -> magnitude -> warp -> peak.
// Paced at ~60 fps so the worker is idle when the next cook arrives, like in TouchDesigner.
// ------------------------------------------------------------------------------------------
static void cookThreadBench(const Args& a, const FFTWEngine& engine, const AlignedVector& window,
                            const PerceptualWarping& warp, size_t n_mag, size_t pad_start)
{
    struct Job { std::vector<AlignedVector> windows; uint64_t seq{ 0 }; clk::time_point published; };
    struct Result { std::vector<AlignedVector> spectra; uint64_t seq{ 0 }; float peakHz{ 0.0f }; };
    const uint32_t kPollMs = 2;                                      // same policy as FFT::workerLoop
    const size_t ch = static_cast<size_t>(a.channels), win = static_cast<size_t>(a.win);
    const size_t N = static_cast<size_t>(a.fft), bins = warp.outputBins();
    const size_t block = 735;                                        // 44.1 kHz @ 60 fps

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
    pickup_us.reserve(static_cast<size_t>(a.cook) + 8);
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
            scaleInPlace(mixed.data(), block, 0.5f);
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
    auto report = [&](const char* label, std::vector<double>& t) {
        std::sort(t.begin(), t.end());
        double sum = 0; for (double v : t) sum += v;
        std::printf("  %-22s mean %6.2f us  median %6.2f us  p99 %6.2f us  max %7.2f us  (n=%zu)\n",
                    label, sum / t.size(), t[t.size() / 2], t[t.size() * 99 / 100], t.back(), t.size());
    };
    // Between cooks TouchDesigner's cook thread is busy with other operators: keep the core awake
    // and clocked up but stream through 8 MB so our working set is evicted from L1/L2 like it is in TD.
    std::vector<float> evict(2 * 1024 * 1024, 1.0f);
    volatile float sink = 0.0f;
    auto pace = [&] {
        auto until = clk::now() + std::chrono::milliseconds(15);
        while (clk::now() < until) { float s = 0; for (size_t i = 0; i < evict.size(); i += 16) s += evict[i]; sink = s; }
    };

    std::thread worker([&] {
        // Match the plugin's worker (nameAndBoostCurrentThread in FFT.cpp) so the simulated
        // pipeline owner thread competes at the same priority level.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        DenormalGuard ftz;
        for (;;) {
            if (stop.load()) return;
            if (jobs.acquire()) { runJob(jobs.front()); continue; }
            wake.waitFor(kPollMs);
        }
    });
    std::vector<double> t_async, t_sync;
    t_async.reserve(static_cast<size_t>(a.cook));
    t_sync.reserve(static_cast<size_t>(a.cook));
    for (int it = 0; it < 5; ++it) { cook(true); pace(); }         // warm-up
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

struct Stage { const char* name; double us = 0.0; };

int main(int argc, char** argv)
{
    // Unbuffered stdout. When this process is piped (CI, a redirected log, rtk) the MSVC CRT block-
    // buffers stdout and abort()/an access violation does not flush it, so a crash loses *every* line
    // printed before it and the run looks like it produced no output at all rather than like it died.
    // The bench prints a few dozen lines outside the timing loop, so line-buffering costs nothing
    // measurable and buys a crash whose last line is the line it died on.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Args a = parse(argc, argv);
    const double sr = 44100.0;
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
    WindowGenerator::generateWindow(0, 15.0, win, window, WindowNorm::CoherentGain);
    PerceptualWarping warp;
    warp.setInterpolation(a.interp);
    const double fmax = a.fmax > sr / 2 ? sr / 2 : a.fmax;
    warp.buildWarpTables(a.scale, fmax, bins, sr / 2.0, 0.963, 20.0, N / 2 + 1);
    const size_t n_mag = std::min(N / 2 + 1, warp.maxLinearIndex() + 1);
    std::printf("warp: %s interpolation, identity=%d, magnitude bins computed %zu of %zu\n",
                a.interp ? "cubic" : "linear", warp.isIdentity() ? 1 : 0, n_mag, N / 2 + 1);
    // Sample rate the CHOP reports for this spectrum: the axis is 0..fmax with the last bin on
    // Nyquist, so it is 2*fmax (see the output rate model in FFT.cpp). When the grid above comes
    // out identity — Linear scale, N/2+1 bins, fmax = nyquist, i.e. the raw FFT grid — that is
    // exactly the input rate. Hz per bin is the mean spacing; a warped grid is not uniform.
    // The band the axis covers, 2 x (top of the axis) — the CHOP reports bins x me.time.rate as
    // info->sampleRate instead, which is a throughput figure this bench has no frame rate for.
    std::printf("axis rate: %.1f Hz (axis 0..%.1f Hz over %zu bins, %.3f Hz per bin on average)\n",
                2.0 * fmax, fmax, bins, bins > 1 ? fmax / static_cast<double>(bins - 1) : 0.0);
    AlignedVector weighting;
    EqualLoudness::computeCurve(1, warp.targetHz(), weighting);

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
    std::vector<float> block(735);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<float>(0.3 * std::sin(i * 0.1) + 0.1 * std::sin(i * 1.7));
    size_t pad_start = ((N > win) ? (N - win) / 2 : 0) & ~static_cast<size_t>(7);

    Stage stages[] = { {"fifo"}, {"eq"}, {"window"}, {"fft+mag"}, {"warp"}, {"weighting"}, {"dB"}, {"ballistics"}, {"peak"} };
    auto tick = [](clk::time_point& t, Stage& s) {
        auto n = clk::now();
        s.us += std::chrono::duration<double, std::micro>(n - t).count();
        t = n;
    };

    // warm-up
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
    for (int it = 0; it < a.iters; ++it) {
        for (auto& c : chans) {
            auto t = clk::now();
            if (a.eq == 1) {
                // current: filter only the new block at ingest (stateful IIR, time order)
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
                float pk = peakMagnitude(c.warped.data(), c.warped.size());
                DecibelConverter::convertToDB(a.db, 80.0, 1.0f / (pk > 0 ? pk : 1.0f), c.warped);
            }
            tick(t, stages[6]);
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
    std::printf("\nper cook (%d channels): %.1f us  = %.2f%% of a 60 fps frame, %.2f%% of a 120 fps frame\n",
                a.channels, per_cook, 100.0 * per_cook / 16666.7, 100.0 * per_cook / 8333.3);

    // ----------------------------------------------------------------------------------------
    // Channel-loop fan-out — the plugin's actual threading design: with more than one analysis
    // channel it runs exactly this loop under std::execution::par, unconditionally (there is no
    // parameter; a single-channel cook has nothing to fan out). Same per-channel pipeline as the
    // stage table above, minus the per-stage clocks, so the two totals line up. Bulk work (the
    // FFT) dominates, so measuring the fan-out here measures what the plugin does.
    // ----------------------------------------------------------------------------------------
    if (a.channels >= 2) {
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
            if (b.size() != serial_out[i].size()) { worst = 1e9; break; }
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

    if (a.cook > 0) cookThreadBench(a, engine, window, warp, n_mag, pad_start);
    return 0;
}
