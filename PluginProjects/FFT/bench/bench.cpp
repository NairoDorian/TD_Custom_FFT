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
#include "RateModel.h"
#include "AnalysisPipeline.h"
#include "AsyncAnalysis.h"

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
    // v2.10: the defaults are the PLUGIN's defaults (1 mono channel, N 16384, dB/weighting/ballistics off),
    // so a run without flags measures what a fresh node does. Pre-2.10 README tables used N 32768 with
    // everything on - pass those flags explicitly to reproduce them.
    int channels = 1, fft = 16384, win = 3175, bins = 16384, scale = 0, iters = 200, db = 0, eq = 0, weight = 0, ball = 0;
    int autoBins = 0;          // --cook/--gate: 1 = Output Bins Mode Auto (N/2+1 of the padded FFT, the plugin default), 0 = Fixed --bins
    int features = 0;          // --cook: Spectral Features on
    int preset = 0;            // --cook: 0 Custom, 1 Visual60, 2 Visual120, 3 Analysis
    std::string gate;          // --gate <baseline.json>: perf regression gate (see perfGate)
    int update = 0;            // --update 1 with --gate: rewrite the baseline
    int privateWisdom = 1;     // --private-wisdom 0: use the user's %LOCALAPPDATA% wisdom (default: a private file)
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


#ifdef _MSC_VER
#pragma warning(disable : 4996)   // fopen: the gate reads/writes one small JSON file, portable C stdio on purpose
#endif

// ===================== COOK-THREAD MEASUREMENT (--cook N) =====================
// v2.10: this drives the REAL code - AnalysisPipeline (the DSP chain) behind AsyncAnalysis (the triple
// buffers, the worker, its wake policy and dormancy handshake) - exactly as FFT::executeImpl does:
// stereo mono-mix ingest -> FIFO -> job slot -> publish -> acquire the newest result -> copy to an
// output buffer of the declared width. Only the TouchDesigner-facing glue (parameter reads, the info
// callbacks) is absent. Cooks are paced (~60 fps) with an 8 MB cache sweep in between, like a busy TD
// cook thread, so the numbers are not flattered by a hot cache or a worker that never sleeps.
//
// Three modes, same parameters: Async + Wake Poll (the default), Async + Wake Signal, Async off.
struct CookStats { double mean, p50, p99, max; AsyncAnalysis::Pickup pickup; uint64_t dropped; };

static Parameters::Values pluginParamsFrom(const Args& a)
{
    Parameters::Values p;                                            // the plugin's defaults ...
    for (int i = 0; i < Parameters::kPadCount; ++i)                  // ... with the bench's overrides
        if (Parameters::kPadValues[i] == a.fft) { p.padIndex = i; p.padSize = a.fft; }
    p.bins = a.bins;
    p.binsMode = a.autoBins ? Parameters::BinsMode::Auto : Parameters::BinsMode::Fixed;
    p.winSamples = a.win;
    p.scale = static_cast<Parameters::Scale>(a.scale);
    p.warpInterp = a.interp ? Parameters::WarpInterp::Cubic : Parameters::WarpInterp::Linear;
    p.loudness = static_cast<Parameters::Loudness>(std::clamp(a.db, 0, 2));
    p.weighting = a.weight ? Parameters::Weighting::AWeighting : Parameters::Weighting::Off;
    p.ballEnable = a.ball != 0;
    p.features = a.features != 0;
    p.planner = static_cast<Parameters::Planner>(static_cast<int>(a.planner));
    p.backend = a.backend ? Parameters::Backend::OneMkl : Parameters::Backend::Fftw3;
    p.displayMax = a.fmax;
    p.preset = static_cast<Parameters::Preset>(std::clamp(a.preset, 0, 3));
    applyPreset(p);
    return p;
}

static CookStats runCooks(Parameters::Values p, bool async, Parameters::WorkerWake wake, int cooks, int paceMs, bool evict)
{
    const double sr = 44100.0;
    const size_t block = 735;                                        // 44.1 kHz @ 60 fps
    p.async = async;
    p.workerWake = wake;
    PlanLog log;
    AnalysisPipeline pipe(&log);
    AsyncAnalysis an(pipe, log);
    an.configure(async, wake, Parameters::WorkerPriority::Highest);
    const int win = windowSamplesFrom(p, sr);
    const size_t bins = static_cast<size_t>(outputBinCountFrom(p, sr));
    FIFOBuffer fifo(static_cast<size_t>(win));
    std::vector<float> inL(block), inR(block);
    for (size_t i = 0; i < block; ++i) { inL[i] = static_cast<float>(0.3 * std::sin(i * 0.1)); inR[i] = static_cast<float>(0.2 * std::sin(i * 0.37)); }
    AlignedVector mixed(block), out(bins);
    std::vector<float> evictBuf(evict ? 2 * 1024 * 1024 : 0, 1.0f);
    volatile float sink = 0.0f;
    uint64_t seq = 0;
    auto cook = [&]() -> double {
        const auto t0 = clk::now();
        DenormalGuard ftz;
        std::memcpy(mixed.data(), inL.data(), block * sizeof(float));
        addInto(mixed.data(), inR.data(), mixed.data(), block);
        scaleInPlace(mixed.data(), block, 0.5f);
        fifo.add(mixed.data(), block);
        AnalysisJob& j = an.jobSlot();
        j.seq = ++seq; j.numChannels = 1; j.sampleRate = sr; j.winSamples = win; j.dtMs = 1000.0 / 60.0; j.p = p; j.reset = false;
        if (j.windows.size() != 1) j.windows.resize(1);
        if (j.silent.size() != 1) j.silent.assign(1, 0);
        fifo.get(j.windows[0]);
        an.publish();
        an.acquireResult();
        const AnalysisResult& r = an.result();
        if (!r.spectra.empty()) {
            const size_t n = std::min(bins, r.spectra[0].size());
            std::memcpy(out.data(), r.spectra[0].data(), n * sizeof(float));
        }
        return std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    };
    auto pace = [&] {
        const auto until = clk::now() + std::chrono::milliseconds(paceMs);
        while (clk::now() < until) {
            if (evict) { float s = 0; for (size_t i = 0; i < evictBuf.size(); i += 16) s += evictBuf[i]; sink = s; }
        }
    };
    for (int i = 0; i < 30; ++i) { cook(); pace(); }                  // warm-up: plan, tables, buffers, worker
    std::vector<double> t;
    t.reserve(static_cast<size_t>(cooks));
    for (int i = 0; i < cooks; ++i) { pace(); t.push_back(cook()); }
    std::sort(t.begin(), t.end());
    double sum = 0; for (double v : t) sum += v;
    CookStats cs{ sum / t.size(), t[t.size() / 2], t[std::min(t.size() - 1, t.size() * 99 / 100)], t.back(), an.pickup(), an.jobsDropped() };
    an.configure(false, wake, Parameters::WorkerPriority::Highest);
    return cs;
}

static void cookThreadBench(const Args& a)
{
    const Parameters::Values p = pluginParamsFrom(a);
    std::printf("\ncook-thread cost per cook (real AsyncAnalysis + AnalysisPipeline; %d output bins, N=%d, stereo mono-mix ingest, "
                "~60 fps pacing, caches evicted between cooks, n=%d):\n",
                outputBinCountFrom(p, 44100.0), p.padSize, a.cook);
    struct Mode { const char* name; bool async; Parameters::WorkerWake wake; };
    const Mode modes[] = { { "Async, Wake Poll", true, Parameters::WorkerWake::Poll },
                           { "Async, Wake Signal", true, Parameters::WorkerWake::Signal },
                           { "Async off (inline)", false, Parameters::WorkerWake::Poll } };
    for (const Mode& m : modes) {
        const CookStats c = runCooks(p, m.async, m.wake, a.cook, 15, true);
        std::printf("  %-20s mean %7.2f us  p50 %7.2f us  p99 %7.2f us  max %8.2f us", m.name, c.mean, c.p50, c.p99, c.max);
        if (m.async) std::printf("  | pickup p50 %.0f us p99 %.0f us, late %llu, dropped %llu",
                                 c.pickup.p50Us, c.pickup.p99Us, static_cast<unsigned long long>(c.pickup.late),
                                 static_cast<unsigned long long>(c.dropped));
        std::printf("\n");
    }
}

// ===================== PERF GATE (--gate <baseline.json> [--update 1]) =====================
// A fixed set of measurements on the real code, compared against a per-machine baseline. Registered
// with ctest under the label "perf" (td_plugin_add_bench ... GATE): `ctest -L perf`. A metric fails
// when it exceeds baseline * 1.30 + 2 us. Each metric is the minimum over 5 rounds of a median.
// --update 1 re-measures and rewrites the baseline (do it on the machine the gate runs on, idle).
// The FFT plans come from the bench's private wisdom file, so the gate never touches the user's.
static bool readBaseline(const char* path, std::vector<std::pair<std::string, double>>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    size_t pos = 0;
    while ((pos = text.find('"', pos)) != std::string::npos) {
        const size_t end = text.find('"', pos + 1);
        if (end == std::string::npos) break;
        const std::string key = text.substr(pos + 1, end - pos - 1);
        const size_t colon = text.find(':', end);
        if (colon == std::string::npos) break;
        size_t v0 = colon + 1;
        while (v0 < text.size() && (text[v0] == ' ' || text[v0] == '	')) ++v0;
        if (v0 < text.size() && text[v0] == '"') {                      // a string value (e.g. "_note"): skip it whole
            const size_t close = text.find('"', v0 + 1);
            if (close == std::string::npos) break;
            pos = close + 1;
            continue;
        }
        char* stop = nullptr;
        const double v = std::strtod(text.c_str() + colon + 1, &stop);
        if (stop != text.c_str() + colon + 1) out.emplace_back(key, v);
        pos = (stop && stop > text.c_str() + colon) ? static_cast<size_t>(stop - text.c_str()) : colon + 1;
    }
    return true;
}

static double pipelineP50(Parameters::Values p, int iters)
{
    p.async = false;
    PlanLog log;
    AnalysisPipeline pipe(&log);
    AnalysisJob job;
    job.numChannels = 1; job.sampleRate = 44100.0; job.winSamples = windowSamplesFrom(p, 44100.0); job.p = p; job.dtMs = 16.7;
    job.windows.assign(1, AlignedVector(static_cast<size_t>(job.winSamples)));
    for (size_t i = 0; i < job.windows[0].size(); ++i) job.windows[0][i] = static_cast<float>(0.3 * std::sin(i * 0.1) + 0.1 * std::sin(i * 1.7));
    job.silent.assign(1, 0);
    AnalysisResult res;
    for (int i = 0; i < 50; ++i) pipe.process(job, res);
    std::vector<double> t(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clk::now();
        pipe.process(job, res);
        t[static_cast<size_t>(i)] = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

static int perfGate(const Args& a)
{
    // Noise control for a laptop: pin to one core at high priority, and score every metric as the
    // MINIMUM of 5 rounds' medians - interference (turbo steps, thermals, background work) only ever
    // makes a round slower, so the minimum is the stable estimate of what the code costs.
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), 1ull << 2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    auto best = [](auto fn) { double b = 1e30; for (int r = 0; r < 5; ++r) b = std::min(b, fn()); return b; };
    std::vector<std::pair<std::string, double>> m;
    Parameters::Values def;                                          // the plugin's defaults (Auto = 8193 bins, beta 15)
    def.planner = Parameters::Planner::Measured;                     // measured plans (from the bench's wisdom)
    Parameters::Values fixed16k = def; fixed16k.rawBins = true;       // the raw rfft path (identity memcpy)
    Parameters::Values v60 = def; v60.preset = Parameters::Preset::Visual60; applyPreset(v60);
    Parameters::Values full = def; full.loudness = Parameters::Loudness::Db; full.weighting = Parameters::Weighting::AWeighting;
    full.ballEnable = true; full.features = true;
    m.emplace_back("pipeline_default_p50_us", best([&] { return pipelineP50(def, 800); }));
    m.emplace_back("pipeline_rawbins_p50_us", best([&] { return pipelineP50(fixed16k, 800); }));
    m.emplace_back("pipeline_visual60_p50_us", best([&] { return pipelineP50(v60, 800); }));
    m.emplace_back("pipeline_fullchain_p50_us", best([&] { return pipelineP50(full, 800); }));
    m.emplace_back("cook_async_poll_p50_us", best([&] { return runCooks(def, true, Parameters::WorkerWake::Poll, 200, 4, false).p50; }));
    m.emplace_back("cook_sync_p50_us", best([&] { return runCooks(def, false, Parameters::WorkerWake::Poll, 200, 4, false).p50; }));

    if (a.update) {
        FILE* f = std::fopen(a.gate.c_str(), "wb");
        if (!f) { std::printf("gate: cannot write %s\n", a.gate.c_str()); return 2; }
        std::fprintf(f, "{\n  \"_note\": \"fft_bench --gate baseline; regenerate with --gate <this file> --update 1 on the gating machine\",\n");
        for (size_t i = 0; i < m.size(); ++i)
            std::fprintf(f, "  \"%s\": %.3f%s\n", m[i].first.c_str(), m[i].second, i + 1 < m.size() ? "," : "");
        std::fprintf(f, "}\n");
        std::fclose(f);
        std::printf("gate: baseline written to %s\n", a.gate.c_str());
        for (auto& kv : m) std::printf("  %-28s %9.3f us\n", kv.first.c_str(), kv.second);
        return 0;
    }
    std::vector<std::pair<std::string, double>> base;
    if (!readBaseline(a.gate.c_str(), base)) { std::printf("gate: cannot read %s\n", a.gate.c_str()); return 2; }
    int fails = 0, compared = 0;
    for (auto& kv : m) {
        const auto it = std::find_if(base.begin(), base.end(), [&](auto& b) { return b.first == kv.first; });
        if (it == base.end()) { std::printf("  %-28s %9.3f us  (no baseline)\n", kv.first.c_str(), kv.second); continue; }
        ++compared;
        const double limit = it->second * 1.30 + 2.0;
        const bool ok = kv.second <= limit;
        if (!ok) ++fails;
        std::printf("  %-28s %9.3f us  baseline %9.3f  limit %9.3f  %s\n", kv.first.c_str(), kv.second, it->second, limit, ok ? "ok" : "REGRESSION");
    }
    if (compared == 0) { std::printf("gate: the baseline has no metrics - run with --update 1 once on this machine\n"); return 0; }
    std::printf("gate: %d regression(s)\n", fails);
    return fails ? 1 : 0;
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
        else if (k == "--auto-bins") a.autoBins = std::atoi(v);
        else if (k == "--features") a.features = std::atoi(v);
        else if (k == "--preset") a.preset = std::atoi(v);
        else if (k == "--gate") a.gate = v;
        else if (k == "--update") a.update = std::atoi(v);
        else if (k == "--private-wisdom") a.privateWisdom = std::atoi(v);
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
    // Private wisdom by default: a bench run must neither depend on nor rewrite the user's plan cache.
    if (a.privateWisdom) FFTWEngine::wisdomPathOverride() = "fft_bench_wisdom.txt";
    if (!a.gate.empty()) return perfGate(a);
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
    if (a.cook > 0) cookThreadBench(a);
    if (a.info > 0) infoPathBench(a.info);
    return 0;
}
