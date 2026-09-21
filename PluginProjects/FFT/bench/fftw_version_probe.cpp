// ===========================================================================================
// fftw_version_probe.cpp - which FFTW is this machine actually running on, and what does one
// transform cost on it?
//
// WHAT: a standalone console program. It prints the fftwf DLL this process loaded (full path and
// size), the version that DLL reports, whether that build exports FFTW's threading API, which SIMD
// codelet family FFTW picks for the plugin's default size, and the plan-build and execute cost of one
// real-to-complex (r2c) transform at every Zero-Pad length the plugin offers.
//
// HOW IT FITS: includes <fftw3.h> (the vendored 3rdParty/fftw3 header) and <windows.h>/<tlhelp32.h>
// for the module-list scan. It includes nothing from source/ and is not called by anything - it is a
// program, and main() below is its entry point. The plugin's own FFT path lives in source/ and is
// deployed as FFT.dll; this probe measures only the library that path calls into.
//
// NOT: it is not built into the plugin and not shipped with it. Build rule: CMakeLists.txt,
// td_plugin_add_bench(), target `fft_version_probe`, guarded by option FFT_BUILD_BENCH (ON).
//
// BUILD AND RUN (all paths relative to PluginProjects/FFT):
//   _c.bat                  configure the standalone tree (Ninja, Release)
//   _b.bat                  build it
//   build/bin/Release/fft_version_probe.exe [--plan auto|fast|measured|patient] [--iters N]
//   Run it from build/bin/Release: that directory holds the vendored DLL
//   (libfftw3f-3.3.11-avx2.dll) the exe resolves against.
//   Name note: the usage line farther down calls this exe "fftw_version_probe"; the CMake target and
//   the file that is actually built are "fft_version_probe".
//
// -------------------------------------------------------------------------------------------
// What FFTW am I actually linked against, which SIMD is that build using, and how fast is one r2c
// transform at the sizes this plugin uses?
//
//   build/bin/Release/fftw_version_probe.exe [--plan auto|fast|measured|patient] [--iters N]
//
// This exists because the plugin reports its *plan* (FFTW_MEASURE / FFTW_PATIENT / ESTIMATE) but never
// the library it runs on, and the two are independent: the FFTW DLL is a vendored binary
// (3rdParty/fftw3), so its version and its SIMD codelet set are a property of that file, not of the
// C++ that calls it. Everything the plugin's per-cook FFT cost depends on is decided there — a
// 3.3.11 build with AVX2 codelets is a different machine from the unversioned `libfftw3f-3.dll` that
// shipped originally, even though the calling code is byte-identical.
//
// A/B procedure (this is what the numbers in 3rdParty/fftw3/README.md were produced with):
//   1. save the DLL under test next to the exe, run it, keep the output
//   2. swap in the other DLL, run it again
//   The exe itself is not rebuilt between the two runs — only the file on disk changes. The module
//   loader resolves the import by *name*, so a substitute DLL must carry the same file name as the one
//   the exe was linked against; the probe prints the real fftwf_version and the resolved full path, so
//   a mislabelled file cannot pass unnoticed.
//
// Everything is resolved at runtime through GetProcAddress rather than linked directly. That is not
// decoration: the DLL this project used before exports the whole fftwf_ surface including the threading
// API, while a `-DENABLE_THREADS=OFF` source build exports only the 78 public functions and no
// threads at all. Linking the threads symbols would make this probe fail to build against exactly the
// library we want to test.
//
// Scope, so the sentence above is not read too widely: the *threading* entry points are the ones
// resolved this way. The rest of the fftwf_ surface this probe calls (alloc, plan, execute, version)
// comes from the import library the bench target links, which is also what makes the FFTW_DLL check
// in printModuleInfo a meaningful test.
//
// Printed, in order:
//   * which fftwf DLL got loaded (full path + size), and the version it reports,
//   * whether that DLL exports the threading API (the plugin deliberately does not use it — see
//     fftw_threads_probe.cpp for the measurement),
//   * the plan FFTW builds for the plugin's default size, printed verbatim, because FFTW puts the
//     codelet names in it — "…_avx2" in there is direct proof of which SIMD kernel is live,
//   * median execute time per r2c transform at every Zero-Pad length the plugin offers, measured with
//     the zero-padding the plugin actually applies (3175 nonzero samples by default, so the number is
//     the real cost, not a fully-populated worst case), normalised by N·log2(N) so an anomaly at one
//     size shows up instead of hiding inside the raw column.

#include <fftw3.h>

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;

namespace {

// ===================== CONSTANTS =====================
// The Zero-Pad lengths this probe sweeps. This list is a duplicate on purpose: the plugin's own copy
// is source/Parameters.h (kPadValues), and this exe deliberately includes nothing from source/.
// Keep the two in step - a size added there and not here goes unmeasured, silently.
constexpr int kPadValues[] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
constexpr int kPadCount = 7;            // must equal the number of entries in kPadValues
constexpr int kDefaultWindow = 3175;    // plugin default Window Sampling (Samples mode)

// ===================== RUNTIME RESOLUTION =====================
// --- runtime resolution -----------------------------------------------------
// The plugin's vendored DLL carries its version in its name (libfftw3f-3.3.11-avx2.dll), so the
// lookup is by prefix scan rather than a fixed string: a future 3.3.12 swap must not require editing
// this probe. The name is not guessable at compile time, but it does not have to be — the import
// table already named a specific file and the loader refused to start without it, so the module is
// guaranteed to be in this process and the only question is which entry it is. Snapshotting the
// loader's module list answers that. (GetModuleHandleExA with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
// is the tidier-looking alternative, but &fftwf_execute under __declspec(dllimport) yields the
// address of this exe's IAT slot, not the DLL's code, so it would report the wrong module.)
//
// Stock fftw3f.dll is accepted too, so the same probe can be pointed at an upstream build.
//
// WHAT:  returns the handle of the fftwf DLL that is already loaded in this process, or nullptr if
//        there is none.
// WHY:   the DLL's file name carries its version, so the name is not a compile-time constant and
//        cannot be looked up as one. Scanning the loaded-module list sidesteps that; the note above
//        explains why the tidier-looking API would not work.
// CALLED BY: printModuleInfo().
// HOW TO CHANGE: the two name tests in the loop are the whole search rule - a DLL named anything else
//        is invisible to this probe, and the probe then reports "not found" rather than an error.
HMODULE fftwModule()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return nullptr;

    HMODULE found = nullptr;
    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            const char* base = me.szModule;   // szModule is the file name only, no path
            if (std::strncmp(base, "libfftw3f-", 10) == 0 || std::strcmp(base, "fftw3f.dll") == 0) {
                found = me.hModule;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// WHAT:  looks one symbol up by name in DLL `h` and returns it as a function pointer of type Fn -
//        nullptr when h is null or the DLL does not export that name.
// WHY:   "not exported" is an expected answer here, not a failure: a build configured
//        -DENABLE_THREADS=OFF exports no threading entry points at all, and the caller simply tests
//        for nullptr. GetProcAddress also returns the true address of a data symbol, which the direct
//        import read cannot be trusted to do on MSVC (see printModuleInfo for that story).
// HOW TO CHANGE: Fn must match the DLL's declared prototype exactly. This cast does not check it and
//        the compiler cannot see across it.
// CALLED BY: printModuleInfo().
template <typename Fn>
Fn resolve(HMODULE h, const char* symbol)
{
    if (!h)
        return nullptr;
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(h, symbol)));
}

// ===================== REPORTING =====================
// WHAT:  prints the FFTW DLL's full path, its size on disk, the version string it reports, whether
//        the two independent readings of that version agree, and whether it exports the threads API.
// WHY:   this is the function that makes the probe's output trustworthy: a substitute DLL whose name
//        does not match its contents, or an exe built with the wrong FFTW_DLL define, both show up
//        here instead of quietly skewing the timings that follow.
// CALLED BY: main(), before anything is planned or timed.
void printModuleInfo()
{
    HMODULE h = fftwModule();
    if (!h) {
        std::printf("FFTW module:   <not found - the DLL is not loaded>\n");
        return;
    }
    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(h, path, MAX_PATH);
    long long bytes = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        bytes = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

    std::printf("FFTW module:   %s\n", path);
    std::printf("  size:        %lld bytes\n", bytes);

    // Two independent reads of the same fact, on purpose.
    //
    // `fftwf_version` is a data symbol, not a function. On MSVC it only resolves correctly when the
    // consumer compiles with FFTW_DLL defined (fftw3.h says so explicitly); without it the name binds
    // to the import library's jump thunk and printf prints the thunk's machine code. MinGW/libtool
    // builds hide that, so the mistake survives a version upgrade and shows up as garbage here.
    // GetProcAddress always returns the real address, so comparing the two is a direct test that the
    // FFTW_DLL define is actually in effect for this target.
    const char* viaImport = fftwf_version;
    const char* viaDlsym  = reinterpret_cast<const char*>(
        reinterpret_cast<void*>(GetProcAddress(h, "fftwf_version")));
    std::printf("  reports:     %s\n", viaImport);
    if (viaDlsym) {
        const bool agree = std::string(viaImport) == std::string(viaDlsym);
        std::printf("  import vs GetProcAddress: %s\n",
                    agree ? "agree (FFTW_DLL is defined, data symbols bind correctly)"
                          : "DISAGREE - FFTW_DLL is not defined for this target, the import read is garbage");
    }

    using InitFn = int (*)(void);
    using WithNtFn = void (*)(int);
    using CleanupFn = void (*)(void);
    const char* threads = "absent";
    if (resolve<InitFn>(h, "fftwf_init_threads") && resolve<WithNtFn>(h, "fftwf_plan_with_nthreads")) {
        threads = "exported (the plugin does not use it - see fftw_threads_probe.cpp)";
    }
    std::printf("  threads API: %s\n", threads);
    (void)sizeof(CleanupFn);   // CleanupFn is declared above but never called; this line is its only use
}

// Which SIMD kernels FFTW chose. FFTW writes the codelet name into the printed plan (the name field
// of fftw_codelet_desc is documented "for debugging only", but it is populated and it is printed), so
// scanning that string is a direct runtime answer instead of an inference from the file's symbols.
//
// WHAT:  builds one throwaway r2c plan of length N, returns FFTW's own printed description of it as a
//        string (empty if no plan could be built), and frees everything it allocated.
// WHY:   that printed text contains the codelet names FFTW picked, so the SIMD family can be reported
//        as something observed rather than something assumed (see the note above).
// HOW TO CHANGE: the returned text is used raw - callers scan it for codelet names, so nothing here
//        may trim, re-wrap or re-case it. The buffer below is filled with a fixed signal rather than a
//        random one so the plan is identical from run to run. N/2 + 1 is the r2c half-spectrum: a real
//        input of N samples produces N/2 + 1 complex bins.
// CALLED BY: main(), for the "SIMD in use" line.
std::string probePlanSimd(int N)
{
    float* in = fftwf_alloc_real(static_cast<size_t>(N));
    fftwf_complex* out = fftwf_alloc_complex(static_cast<size_t>(N) / 2 + 1);
    // 0.01 rad/sample is an arbitrary fixed tone, and only the first kDefaultWindow samples are
    // non-zero - the same signal the plugin would see. (tone value not derived in this file)
    for (int i = 0; i < N; ++i)
        in[i] = (i < kDefaultWindow) ? static_cast<float>(std::sin(i * 0.01)) : 0.0f;

    fftwf_plan p = fftwf_plan_dft_r2c_1d(N, in, out, FFTW_ESTIMATE);
    std::string plan = p ? fftwf_sprint_plan(p) : std::string();
    if (p)
        fftwf_destroy_plan(p);
    fftwf_free(in);
    fftwf_free(out);
    return plan;
}

// ===================== PLAN POLICY AND TIMING =====================
// FFTW_MEASURE is 0U, not a bit. ESTIMATE / PATIENT / EXHAUSTIVE are the flags that can actually be
// set, and MEASURE is what you get when none of them is. So testing `rigor & FFTW_MEASURE` first —
// the order that looks obvious, and that this file originally had — is always false and reports
// every measured plan as "FFTW_ESTIMATE", which is the one thing you must not get wrong when the
// whole point is telling policies apart.
//
// WHAT:  turns an FFTW planner-rigor flag set into a short label ("FFTW_PATIENT"), appending
//        "|WISDOM_ONLY" when that bit is set too.
// WHY:   the flag order below is the opposite of the obvious one, for the reason given above.
// CALLED BY: main(), once for the "policy:" line and once per row of the size table.
std::string policyName(unsigned rigor)
{
    const char* base;
    if (rigor & FFTW_EXHAUSTIVE)    base = "FFTW_EXHAUSTIVE";
    else if (rigor & FFTW_PATIENT)  base = "FFTW_PATIENT";
    else if (rigor & FFTW_ESTIMATE) base = "FFTW_ESTIMATE";
    else                            base = "FFTW_MEASURE";
    return (rigor & FFTW_WISDOM_ONLY) ? (std::string(base) + "|WISDOM_ONLY") : std::string(base);
}

// WHAT:  executes `plan` `iters` times, times each execution on its own, and returns the median in
//        microseconds. `plan` must be non-null: every caller supplies a plan it already checked.
// WHY:   the median and not the mean: this runs on a desktop that may be running TouchDesigner at the
//        same time, and one preempted iteration must not move a number that is going to be written
//        down. The 10 untimed executions first only warm caches and first-touch the pages; that count
//        is not derived in this file.
// HOW TO CHANGE: allocates a vector of `iters` doubles per call - fine in this program, but never
//        call it from anything on the plugin's cook thread.
// CALLED BY: main(), for every timing it prints.
double medianUs(fftwf_plan p, int iters)
{
    std::vector<double> t;
    t.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < 10; ++i)
        fftwf_execute(p);                                  // warm caches, first-touch pages
    for (int i = 0; i < iters; ++i) {
        const auto a = clk::now();
        fftwf_execute(p);
        const auto b = clk::now();
        t.push_back(std::chrono::duration<double, std::micro>(b - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

} // namespace

// ===================== MAIN =====================
// WHAT:  the entry point. It parses --iters and --plan, prints the module / policy / SIMD report, then
//        prints one table row per Zero-Pad length the plugin offers (how long the plan took to build,
//        and the median transform time), and finally one line for the plugin's default configuration.
// HOW TO CHANGE: three things must stay in step with the plugin, none of which is checked by the
//        compiler - kPadValues and kPadCount above (plugin copy: source/Parameters.h kPadValues), the
//        default window size (source/Parameters.cpp, Window Sampling = 3175), and the 16384 used for
//        the SIMD line and the default-config block (source/Parameters.h, kPadDefault).
// CALLED BY: the C runtime; this is the program's entry point.
int main(int argc, char** argv)
{
    unsigned rigor = FFTW_MEASURE;
    int iters = 300;   // 300 not derived in this file; only has to be enough that one slow run cannot move a median
    for (int i = 1; i + 1 < argc; i += 2) {   // pairs: stops before a trailing flag with no value
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--iters") {
            iters = std::atoi(v.c_str());
        } else if (k == "--plan") {
            // These four names are the plugin's own planner policies (src/DSPModules.h, PlannerPolicy),
            // and the default "auto" is deliberately the same flag set the plugin's Auto policy uses:
            // FFTW_MEASURE | FFTW_WISDOM_ONLY, i.e. "reuse a measured plan if wisdom has one, and do not
            // stop to measure if it does not". A wisdom miss returns no plan; the table below notices
            // that and re-plans with ESTIMATE, which is also what the plugin does. So a row printed as
            // "ESTIMATE (wisdom miss)" is a faithful picture of the first cook on a cold machine.
            if (v == "fast")          rigor = FFTW_ESTIMATE;
            else if (v == "patient")  rigor = FFTW_PATIENT;
            else if (v == "measured") rigor = FFTW_MEASURE;
            else                      rigor = FFTW_MEASURE | FFTW_WISDOM_ONLY;
        }
    }

    printModuleInfo();
    std::printf("  policy:      %s\n\n", policyName(rigor).c_str());

    // Report the SIMD family the library actually picked for the plugin's default size.
    // 16384 is that default (source/Parameters.h, kPadDefault), and the literal is repeated in the
    // printf below - change both or the line contradicts the plan it just printed.
    {
        const std::string plan = probePlanSimd(16384);
        const char* family = "none recognised";
        if (plan.find("_avx2") != std::string::npos)       family = "AVX2 (256-bit vectors, FMA-capable codelets)";
        else if (plan.find("_avx") != std::string::npos)   family = "AVX (128-bit vectors for single precision)";
        else if (plan.find("_sse2") != std::string::npos)  family = "SSE2 (128-bit)";
        std::printf("SIMD in use at N = 16384: %s\n", family);
        if (!plan.empty()) {
            // The plan is a multi-line tree; its first line is just the top-level node
            // ("(rdft2-ct-dit/32") and carries no codelet name. Print the first line that does, so
            // the output shows the evidence rather than an assertion about it.
            size_t pos = 0;
            bool shown = false;
            while (pos < plan.size()) {
                size_t nl = plan.find('\n', pos);
                const std::string line = plan.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                if (line.find("_avx2") != std::string::npos || line.find("_avx") != std::string::npos ||
                    line.find("_sse2") != std::string::npos) {
                    std::printf("  codelet: %s\n", line.c_str());
                    shown = true;
                    break;
                }
                if (nl == std::string::npos)
                    break;
                pos = nl + 1;
            }
            if (!shown)
                std::printf("  codelet: <none named in the plan>\n");
            std::printf("  plan lines: %zu\n",
                        static_cast<size_t>(std::count(plan.begin(), plan.end(), '\n')) + 1);
        }
        std::printf("\n");
    }

    // The size table. Columns: N is the transform length; "plan used" is the planner that actually
    // produced the plan (policyName above, or the ESTIMATE fallback); "plan ms" is how long building
    // the plan took - a one-off cost; "median us" is the per-transform cost that is paid every cook;
    // the last column is that same cost normalised, so an anomaly at one size shows up instead of
    // hiding inside a raw column that naturally grows with N.
    std::printf("%-8s %-24s %10s %12s %14s\n", "N", "plan used", "plan ms", "median us", "us/N*log2N");
    for (int pi = 0; pi < kPadCount; ++pi) {
        const int N = kPadValues[pi];
        float* in = fftwf_alloc_real(static_cast<size_t>(N));
        fftwf_complex* out = fftwf_alloc_complex(static_cast<size_t>(N) / 2 + 1);
        for (int i = 0; i < N; ++i)
            in[i] = (i < kDefaultWindow) ? static_cast<float>(std::sin(i * 0.01)) : 0.0f;

        std::string used = policyName(rigor);
        const auto t0 = clk::now();
        fftwf_plan p = fftwf_plan_dft_r2c_1d(N, in, out, rigor);
        double plan_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        if (!p) {
            // WISDOM_ONLY miss (the plan is not in the wisdom file yet), or a policy that could not
            // plan this size at all. ESTIMATE always succeeds, so the table stays complete.
            used = "ESTIMATE (wisdom miss)";
            p = fftwf_plan_dft_r2c_1d(N, in, out, FFTW_ESTIMATE);
            plan_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        }
        const double us = medianUs(p, iters);
        // x1e6 converts microseconds to picoseconds, so this column reads as ps per N*log2(N).
        const double norm = us / (static_cast<double>(N) * std::log2(static_cast<double>(N))) * 1e6;
        std::printf("%-8d %-24s %10.2f %12.2f %14.4f\n", N, used.c_str(), plan_ms, us, norm);
        fftwf_destroy_plan(p);
        fftwf_free(in);
        fftwf_free(out);
    }

    // The plugin's default configuration: a 16384-point frame holding a 3175-sample window.
    // Both are the plugin's own defaults: 16384 is kPadDefault (source/Parameters.h) and 3175 is the
    // Window Sampling default (source/Parameters.cpp). kDefaultWindow above is the same 3175.
    {
        const int N = 16384;
        float* in = fftwf_alloc_real(N);
        fftwf_complex* out = fftwf_alloc_complex(N / 2 + 1);
        for (int i = 0; i < N; ++i)
            in[i] = (i < kDefaultWindow) ? static_cast<float>(std::sin(i * 0.01)) : 0.0f;
        fftwf_plan p = fftwf_plan_dft_r2c_1d(N, in, out, rigor);
        if (!p)
            p = fftwf_plan_dft_r2c_1d(N, in, out, FFTW_ESTIMATE);   // same wisdom-miss fallback as the table above
        const double us = medianUs(p, iters);
        // 16666.7 and 8333.3 are one frame in microseconds at 60 and 120 fps (1e6 / 60 and 1e6 / 120),
        // so the two percentages say how much of a frame budget a single transform consumes.
        std::printf("\nplugin default (N = 16384, %d-sample window): %.2f us per transform"
                    " = %.2f %% of a 60 fps frame, %.2f %% of a 120 fps frame\n",
                    kDefaultWindow, us, 100.0 * us / 16666.7, 100.0 * us / 8333.3);
        fftwf_destroy_plan(p);
        fftwf_free(in);
        fftwf_free(out);
    }
    return 0;
}
