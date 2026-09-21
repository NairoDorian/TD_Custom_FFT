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

constexpr int kPadValues[] = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
constexpr int kPadCount = 7;
constexpr int kDefaultWindow = 3175;    // plugin default Window Sampling (Samples mode)

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

template <typename Fn>
Fn resolve(HMODULE h, const char* symbol)
{
    if (!h)
        return nullptr;
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(h, symbol)));
}

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
    (void)sizeof(CleanupFn);
}

// Which SIMD kernels FFTW chose. FFTW writes the codelet name into the printed plan (the name field
// of fftw_codelet_desc is documented "for debugging only", but it is populated and it is printed), so
// scanning that string is a direct runtime answer instead of an inference from the file's symbols.
std::string probePlanSimd(int N)
{
    float* in = fftwf_alloc_real(static_cast<size_t>(N));
    fftwf_complex* out = fftwf_alloc_complex(static_cast<size_t>(N) / 2 + 1);
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

// FFTW_MEASURE is 0U, not a bit. ESTIMATE / PATIENT / EXHAUSTIVE are the flags that can actually be
// set, and MEASURE is what you get when none of them is. So testing `rigor & FFTW_MEASURE` first —
// the order that looks obvious, and that this file originally had — is always false and reports
// every measured plan as "FFTW_ESTIMATE", which is the one thing you must not get wrong when the
// whole point is telling policies apart.
std::string policyName(unsigned rigor)
{
    const char* base;
    if (rigor & FFTW_EXHAUSTIVE)    base = "FFTW_EXHAUSTIVE";
    else if (rigor & FFTW_PATIENT)  base = "FFTW_PATIENT";
    else if (rigor & FFTW_ESTIMATE) base = "FFTW_ESTIMATE";
    else                            base = "FFTW_MEASURE";
    return (rigor & FFTW_WISDOM_ONLY) ? (std::string(base) + "|WISDOM_ONLY") : std::string(base);
}

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

int main(int argc, char** argv)
{
    unsigned rigor = FFTW_MEASURE;
    int iters = 300;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--iters") {
            iters = std::atoi(v.c_str());
        } else if (k == "--plan") {
            if (v == "fast")          rigor = FFTW_ESTIMATE;
            else if (v == "patient")  rigor = FFTW_PATIENT;
            else if (v == "measured") rigor = FFTW_MEASURE;
            else                      rigor = FFTW_MEASURE | FFTW_WISDOM_ONLY;
        }
    }

    printModuleInfo();
    std::printf("  policy:      %s\n\n", policyName(rigor).c_str());

    // Report the SIMD family the library actually picked for the plugin's default size.
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
        const double norm = us / (static_cast<double>(N) * std::log2(static_cast<double>(N))) * 1e6;
        std::printf("%-8d %-24s %10.2f %12.2f %14.4f\n", N, used.c_str(), plan_ms, us, norm);
        fftwf_destroy_plan(p);
        fftwf_free(in);
        fftwf_free(out);
    }

    // The plugin's default configuration: a 16384-point frame holding a 3175-sample window.
    {
        const int N = 16384;
        float* in = fftwf_alloc_real(N);
        fftwf_complex* out = fftwf_alloc_complex(N / 2 + 1);
        for (int i = 0; i < N; ++i)
            in[i] = (i < kDefaultWindow) ? static_cast<float>(std::sin(i * 0.01)) : 0.0f;
        fftwf_plan p = fftwf_plan_dft_r2c_1d(N, in, out, rigor);
        if (!p)
            p = fftwf_plan_dft_r2c_1d(N, in, out, FFTW_ESTIMATE);
        const double us = medianUs(p, iters);
        std::printf("\nplugin default (N = 16384, %d-sample window): %.2f us per transform"
                    " = %.2f %% of a 60 fps frame, %.2f %% of a 120 fps frame\n",
                    kDefaultWindow, us, 100.0 * us / 16666.7, 100.0 * us / 8333.3);
        fftwf_destroy_plan(p);
        fftwf_free(in);
        fftwf_free(out);
    }
    return 0;
}
