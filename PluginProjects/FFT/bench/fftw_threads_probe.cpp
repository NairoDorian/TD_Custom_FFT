// ===========================================================================================
// fftw_threads_probe.cpp - does FFTW's built-in threading help this plugin?
//
// WHAT: a standalone console program. It times one 1-D real-to-complex (r2c) transform at several
// FFTW thread counts, then times a batch of four simultaneous transforms the same way, and prints the
// percentage change against the single-threaded baseline. Run with no argument it prints both planner
// rows; with --measure it prints only the MEASURE rows (see the flag's note in main()).
//
// HOW IT FITS: includes <fftw3.h> (the vendored 3rdParty/fftw3 header) and <windows.h>/<tlhelp32.h>
// for the module-list scan, and nothing from source/. It is a program, not a library: main() below is
// its entry point and nothing in the plugin calls it. The plugin's real FFT path is in source/,
// deployed as FFT.dll.
//
// NOT: it is not built into the plugin and not shipped with it. Build rule: CMakeLists.txt,
// td_plugin_add_bench(), target `fft_threads_probe`, guarded by option FFT_BUILD_BENCH (ON).
//
// BUILD AND RUN (all paths relative to PluginProjects/FFT):
//   _c.bat                  configure the standalone tree (Ninja, Release)
//   _b.bat                  build it
//   build/bin/Release/fft_threads_probe.exe [--measure]
//   Run it from build/bin/Release: that directory holds the vendored DLL
//   (libfftw3f-3.3.11-avx2.dll) the exe resolves against.
//   Name note: the source file is fftw_threads_probe.cpp, but the CMake target and the built exe are
//   `fft_threads_probe` - one 'w' shorter.
//
// -------------------------------------------------------------------------------------------
// Does FFTW's built-in threading help this plugin? Measured answer: no.
//
//   build/bin/Release/fft_threads_probe.exe
//
// FFTW's threaded implementation parallelizes the `howmany` loop of a rank-1 (many-arrays) plan and
// multi-dimensional transforms — NOT the inside of a single 1-D transform. The plugin's default
// configuration (Channels = Mono Mix) issues exactly one 1-D R2C transform per cook, so handing
// FFTW threads there only buys thread hand-off cost. The unversioned `libfftw3f-3.dll` this project
// used before did export fftwf_init_threads / fftwf_plan_with_nthreads, so this was worth measuring
// rather than assuming; the current 3.3.11 build is configured -DENABLE_THREADS=OFF and exports
// neither, which costs the plugin nothing — it never called them.
//
// Result on an i7-class desktop, N = 32768 with the plugin's zero-padding (3175 nonzeros), run
// twice on a machine where TouchDesigner was also running (so absolute numbers drift ~15 %; the
// sign and rough size of every delta reproduced):
//
//   single 1-D R2C      nthreads=1   68.8 / 79.7 us    nthreads=2   85.3 / 95.2 us   (+19..24 %)
//                       nthreads=4   81.7 / 93.8 us    nthreads=6  102.5 / 121.1 us  (+18..52 %)
//   howmany = 4 arrays  nthreads=1    86.3 / 97.0 us/tx nthreads=2    48.3 / 55.8 us/tx  (-36..42 %)
//                       nthreads=4    29.9 / 38.2 us/tx                      (-56..61 %)
//
// So threading is a real lever for Channels = All Channels (>= 2 transforms per cook), but it needs
// a batched fftwf_plan_many_dft_r2c plan, which FFTWEngine does not currently build. Re-run this
// probe on any machine before trusting the numbers above.
//
// The threads API is resolved with GetProcAddress rather than linked, because whether it exists at all
// depends on how the DLL was built: the unversioned DLL this project used before exported it, and the
// current 3.3.11 source build is configured -DENABLE_THREADS=OFF and exports no part of it. Linking
// the symbols directly would make this probe fail to build against the DLL it is meant to measure.
// When the API is absent the probe says so, runs the single-threaded baselines, and exits 0 — an
// FFTW without threads is a valid answer to "does FFTW threading help?", not an error.

#include <fftw3.h>

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using clk = std::chrono::steady_clock;

namespace {

// ===================== THREADS API LOOKUP =====================
// Resolved once at startup; all three are null together when the DLL was built without threads.
int  (*g_initThreads)(void)      = nullptr;
void (*g_planWithNthreads)(int)  = nullptr;
void (*g_cleanupThreads)(void)   = nullptr;

// The vendored DLL's file name carries its version (libfftw3f-3.3.11-avx2.dll), so it cannot be
// hardcoded here without pinning this probe to one release. Scan the loader's module list for the
// libfftw3f- prefix instead; "fftw3f.dll" is accepted so the probe also runs against a stock build.
//
// WHAT:  finds the loaded fftwf DLL and fills in the three global function pointers from it. Each one
//        is left null if the DLL does not export it.
// WHY:   whether these symbols exist at all depends on how the DLL was built, so the lookup cannot be
//        done by the linker (see the header). A DLL that was built without threads leaves all three
//        null, which the caller reads as "no threading API".
// CALLED BY: main(), once, before any plan is built.
// HOW TO CHANGE: add a fourth entry point to the same three-step pattern - declare the pointer above,
//        resolve it here, and check it in main's haveThreads test, or it will be used unguarded.
void resolveThreadsApi()
{
    HMODULE h = nullptr;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me{};
        me.dwSize = sizeof(me);
        if (Module32First(snap, &me)) {
            do {
                if (std::strncmp(me.szModule, "libfftw3f-", 10) == 0 ||
                    std::strcmp(me.szModule, "fftw3f.dll") == 0) {
                    h = me.hModule;
                    break;
                }
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
    }
    if (!h)
        return;
    g_initThreads     = reinterpret_cast<int (*)(void)>(reinterpret_cast<void*>(GetProcAddress(h, "fftwf_init_threads")));
    g_planWithNthreads = reinterpret_cast<void (*)(int)>(reinterpret_cast<void*>(GetProcAddress(h, "fftwf_plan_with_nthreads")));
    g_cleanupThreads  = reinterpret_cast<void (*)(void)>(reinterpret_cast<void*>(GetProcAddress(h, "fftwf_cleanup_threads")));
}

} // namespace

// ===================== TIMING =====================
// WHAT:  executes `plan` `iters` times and returns the average cost of one execution in microseconds.
//        `plan` must be non-null - the callers pass the plan straight out of the planner with no null
//        check, so this only ever runs because the flags in use cannot fail to produce one.
// WHY:   this is a mean over one long timed stretch, not a per-run median, so a preempted iteration
//        does move the result - which is why the header tells you to re-run this probe on your own
//        machine before trusting its numbers. The 5 untimed executions first only warm up; that count
//        is not derived in this file.
// HOW TO CHANGE: the division by `iters` is what makes the unit microseconds per call - if the loop
//        ever times more than one transform, the caller must divide again (the rank-1 section does).
// CALLED BY: main(), for every number it prints.
static double timeMany(fftwf_plan p, int iters)
{
    for (int i = 0; i < 5; ++i) fftwf_execute(p);              // warm up
    auto t0 = clk::now();
    for (int i = 0; i < iters; ++i) fftwf_execute(p);
    auto t1 = clk::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
}

// ===================== MAIN =====================
// WHAT:  the entry point. It resolves the threads API, prints whether that API exists, then prints two
//        tables - a single 1-D r2c transform per thread count, and a batch of `howmany` simultaneous
//        transforms per thread count - each as a percentage change against its own nthreads=1 row.
// WHY:   the two tables are separate because they answer different questions: the first is the shape
//        the plugin runs today, the second is the shape FFTW's threading can actually accelerate (see
//        the header for those measurements and what they mean for Channels = All Channels).
// CALLED BY: the C runtime; this is the program's entry point.
int main(int argc, char** argv)
{
    // N is one of the plugin's Zero-Pad lengths and the size the header's result table was measured
    // at; nonzeros is the plugin's window length, i.e. the zero-padding it really applies; howmany is
    // the batch size for the rank-1 section (the header's "howmany = 4 arrays"). iters = 300 and the
    // thread counts below are not derived in this file.
    const int N = 32768, howmany = 4, iters = 300, nonzeros = 3175;
    (void)howmany;   // used only by the rank-1 section below
    // The plugin's real configuration uses a MEASURED plan (Auto upgrades in the background), and FFTW
    // measures *with* the thread count in effect, so ESTIMATE alone would not be a fair test.
    const unsigned rigors[] = { FFTW_ESTIMATE, FFTW_MEASURE };
    const char* rigor_names[] = { "ESTIMATE", "MEASURE" };
    // --measure drops the ESTIMATE rows and keeps only the MEASURE ones, halving the run time. The
    // MEASURE rows are the ones that matter (see the comment on the rank-1 loop below), so this is a
    // quick re-check switch, not a different experiment. Any other argument - or none - measures both.
    const bool only_measure = (argc > 1 && std::string(argv[1]) == "--measure");

    std::printf("FFTW %s\n", fftwf_version);
    resolveThreadsApi();
    // The last term of the && chain is not a null check: g_initThreads() is the actual call that turns
    // FFTW's thread system on, and it only runs because the first three terms proved the symbols exist
    // (short-circuit). Its int result is read as the boolean "threading is usable here".
    const bool haveThreads = g_initThreads && g_planWithNthreads && g_cleanupThreads && g_initThreads();
    if (!haveThreads) {
        std::printf("threads API: absent (this DLL was built without threads, so the nthreads rows are\n"
                    "             skipped; the nthreads=1 baselines below are still valid and are the\n"
                    "             configuration the plugin actually runs in)\n");
    } else {
        std::printf("threads API: present\n");
    }
    // When the API is missing, only the nthreads=1 row of each table can be produced. These are the
    // thread counts the header's result table reports, for the single-transform and batch tables
    // respectively; each loop below stops early when the API is absent.
    const int threadCounts[] = { 1, 2, 4, 6 };
    const int threadCountsRank1[] = { 1, 2, 4 };

    // nc is the r2c output length: a real input of N samples yields N/2 + 1 complex bins. Both the
    // single-plan and the batch section below allocate from it. 0.01 rad/sample is an arbitrary fixed
    // tone (value not derived in this file); only the first `nonzeros` samples are non-zero, which is
    // the zero-padding the plugin actually applies.
    const int nc = N / 2 + 1;
    float* in = fftwf_alloc_real(N);
    fftwf_complex* out = fftwf_alloc_complex(nc);
    for (int i = 0; i < N; ++i) in[i] = (i < nonzeros) ? std::sin(i * 0.01f) : 0.0f;

    // Table 1: one transform at a time - the shape the plugin runs in Mono Mix. Each row is printed as
    // a percentage change against this table's own nthreads=1 row, so `base` is that row's time.
    for (int r = only_measure ? 1 : 0; r < 2; ++r) {   // r walks rigors[]; --measure starts it at MEASURE
        const unsigned rigor = rigors[r];
        std::printf("\nsingle 1-D R2C, N = %d, %d nonzero, planner = %s\n", N, nonzeros, rigor_names[r]);
        double base = 0.0;
        for (int nth : threadCounts) {
            if (nth != 1 && !haveThreads)
                break;
            if (haveThreads)
                g_planWithNthreads(nth);
            fftwf_plan p = fftwf_plan_dft_r2c_1d(N, in, out, rigor);
            double us = timeMany(p, iters);
            if (nth == 1) base = us;
            std::printf("  nthreads=%d  %8.2f us   %+.0f%%\n", nth, us, 100.0 * (us - base) / base);
            fftwf_destroy_plan(p);
        }
    }

    // Table 2: the same signal replicated into a batch of `howmany` arrays, planned as a single
    // many-array (rank-1) plan. This is the shape FFTW's threading can split across threads; it is not
    // the shape the plugin builds today. inM/outM are one contiguous buffer holding every array.
    float* inM = fftwf_alloc_real(static_cast<size_t>(N) * howmany);
    fftwf_complex* outM = fftwf_alloc_complex(static_cast<size_t>(nc) * howmany);
    for (int m = 0; m < howmany; ++m)
        for (int i = 0; i < N; ++i) inM[static_cast<size_t>(m) * N + i] = in[i];
    for (int r = only_measure ? 1 : 0; r < 2; ++r) {
        // A planned (MEASURE) thread split is the only one that matters: the plugin upgrades its plan
        // in the background, so this is the configuration it actually runs with.
        // 30 is the iteration count used when the planner is MEASURE, where `iters` is used otherwise;
        // the value 30 is not derived in this file.
        const int mi = (rigors[r] == FFTW_MEASURE) ? 30 : iters;
        std::printf("\nrank-1 loop: %d simultaneous transforms of N = %d, planner = %s (what FFTW splits)\n",
                    howmany, N, rigor_names[r]);
        double baseM = 0.0;
        for (int nth : threadCountsRank1) {
            if (nth != 1 && !haveThreads)
                break;
            if (haveThreads)
                g_planWithNthreads(nth);
            // The many-array plan, read left to right: rank 1; the single size N; `howmany` transforms;
            // input stride 1 with each transform N samples after the last; output stride 1 with each
            // transform nc bins after the last. The two nulls are the "embed" arguments, which say the
            // arrays are tightly packed and the size comes from n.
            fftwf_plan p = fftwf_plan_many_dft_r2c(1, &N, howmany, inM, nullptr, 1, N,
                                                   outM, nullptr, 1, nc, rigors[r]);
            // timeMany returns microseconds for the whole batch of howmany transforms, so divide by
            // howmany to print a per-transform number that can be compared with table 1.
            double us = timeMany(p, mi) / howmany;
            if (nth == 1) baseM = us;
            std::printf("  nthreads=%d  %7.2f us per transform   %+.0f%%\n", nth, us, 100.0 * (us - baseM) / baseM);
            fftwf_destroy_plan(p);
        }
    }
    // Tear-down: the thread system is shut down only if this run turned it on (g_initThreads() above),
    // and every buffer freed here is one this exe allocated. `in` is held until after table 2 - that
    // table's fill loop copies from it.
    if (haveThreads)
        g_cleanupThreads();
    fftwf_free(in); fftwf_free(out); fftwf_free(inM); fftwf_free(outM);
    return 0;
}
