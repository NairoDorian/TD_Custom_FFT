#ifndef FFT_BACKEND_H
#define FFT_BACKEND_H

/*
===========================================================================
        FFT BACKEND: interchangeable FFTW3-ABI libraries, chosen at runtime
===========================================================================
Header File: FftBackend.h

Why this exists
---------------
Two different libraries can serve this plugin through the same C API:

  * FFTW3 itself, built from source with AVX2 + FMA codelets (3rdParty/fftw3), and
  * Intel oneMKL's FFTW3 compatibility interface, whose kernels are hand-written in assembly and
    dispatched per microarchitecture by Intel for Intel.

They export the *same* fftwf_ symbols, which is the whole point of the compatibility interface - and
also the reason both cannot simply be linked into one binary. The linker would take whichever import
library it saw first and there would be no way to change the choice after the fact, no way to fall
back when one is missing, and no way to compare them inside one process.

So neither is linked. This header resolves the chosen library's entry points at run time and the
engine calls through the resulting table. That buys three things beyond the choice itself:

  * A missing or mismatched DLL is a log line, not a load failure. A statically imported FFTW makes
    the whole plugin refuse to load in TouchDesigner if the DLL is absent; a failed dynamic load
    leaves the node reporting why.
  * The backend can be changed while TouchDesigner is running (the plugin rebuilds its plan against
    the new library), which is what makes a parameter meaningful.
  * It retires the FFTW_DLL / __declspec(dllimport) trap documented in the previous version of this
    file. fftwf_version and fftwf_cc are *data*, and under MSVC a static import of a data symbol
    binds to the import library's jump thunk unless FFTW_DLL is defined, so reading them printed
    machine code. GetProcAddress returns the real address of the array, so the question cannot arise.

What is deliberately NOT here
-----------------------------
No planning policy is defined here. The FFTW plan rigour flags (FFTW_ESTIMATE / MEASURE / PATIENT)
are an FFTW concept: a library that plans by table lookup rather than by measurement ignores them.
Each backend declares whether it honours them (FftBackendInfo::honoursPolicy) so the engine can say
so in the log instead of silently presenting a dropdown that does nothing.
===========================================================================
*/

#ifdef TD_PLUGIN_HAS_FFTW3

// Types only. fftwf_plan is an incomplete struct pointer and fftwf_complex is a typedef; neither
// needs the import library, and nothing in this header is linked against one.
#include <fftw3.h>

#include <cstdio>
#include <cstdlib>      // std::free - for the string fftwf_sprint_plan hands back (see section 4)
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace FFTDSP {

// ---------------------------------------------------------------------------------------------
// 1. The ABI subset the plugin uses
// ---------------------------------------------------------------------------------------------
// Every signature below is copied from the vendored fftw3.h (which generates them from the
// FFTW_DEFINE_API macro, so they do not appear literally in the file). They are spelled with the
// real fftwf_ types rather than void* so that a wrong entry in the table is a compile error rather
// than a crash at the first cook.
struct FftApi {
    // ---- functions the plugin needs; a backend is unusable unless all of these resolve ----
    void* (*alloc)(size_t)                             = nullptr;  // fftwf_malloc
    void  (*dealloc)(void*)                            = nullptr;  // fftwf_free
    fftwf_plan (*planR2C)(int, float*, fftwf_complex*, unsigned) = nullptr;  // fftwf_plan_dft_r2c_1d
    void  (*executeR2C)(const fftwf_plan, float*, fftwf_complex*) = nullptr; // fftwf_execute_dft_r2c
    void  (*destroyPlan)(fftwf_plan)                   = nullptr;  // fftwf_destroy_plan

    // ---- optional; present only if the library supports wisdom ----
    // MKL's FFTW3 interface accepts these calls but has no wisdom to save; it is reported rather
    // than assumed so the log does not claim a plan was cached when it was not.
    int   (*importWisdom)(const char*)                 = nullptr;  // fftwf_import_wisdom_from_filename
    int   (*exportWisdom)(const char*)                 = nullptr;  // fftwf_export_wisdom_to_filename
    char* (*sprintPlan)(const fftwf_plan)              = nullptr;  // fftwf_sprint_plan

    // ---- optional; the plugin does not plan in parallel, but the version report states whether
    // the API exists, because it changes what the library can do ----
    int   (*initThreads)(void)                         = nullptr;  // fftwf_init_threads
    void  (*planWithNthreads)(int)                     = nullptr;  // fftwf_plan_with_nthreads
    // Not an fftwf_ symbol at all: oneMKL's own switch for which threading layer the library loads.
    // Resolved here because it is the only way to keep oneMKL out of the host's OpenMP runtime - see
    // FftBackendInfo::forceSequentialThreading for why that matters. nullptr on FFTW3, which has no
    // such concept (fftwf_init_threads / plan_with_nthreads are its knobs instead).
    int   (*setThreadingLayer)(int)                    = nullptr;  // MKL_Set_Threading_Layer

    // ---- data symbols, not functions ----
    // Resolved by address. For a data export GetProcAddress returns the address of the array, so
    // these can be read directly as C strings.
    const char* version  = nullptr;   // fftwf_version, e.g. "fftw-3.3.11-sse2-avx-avx2-avx2_128"
    const char* compiler = nullptr;   // fftwf_cc, the compile command line

    // Deliberately absent, and this comment is the reason: fftwf_cleanup() and
    // fftwf_forget_wisdom() free state that is *process-global* - every plan, every cached
    // trigonometric table, the whole wisdom set. There is no such thing as cleaning up "this node's"
    // FFTW state, so calling either from a plugin that can be instanced would invalidate the plans
    // other FFT CHOPs are executing with, and the manual's "the only thread-safe routine is
    // fftw_execute" rule means the damage would land in whichever cook thread ran next. A plan is
    // released with destroyPlan() and wisdom is simply left in memory; nothing here needs cleanup.
    // They are not resolved above on purpose, so there is no way to call them by accident.

    // A backend without any of the required five cannot run a transform at all.
    bool complete() const {
        return alloc && dealloc && planR2C && executeR2C && destroyPlan;
    }
    // Wisdom is optional, but the two halves must agree: importing without exporting would make
    // every MEASURE plan cost full price on every run of the host application.
    bool hasWisdom() const { return importWisdom && exportWisdom; }
};

// ---------------------------------------------------------------------------------------------
// 2. Backend descriptors
// ---------------------------------------------------------------------------------------------
struct FftBackendInfo {
    const char*        id;             // stable key, used by parameters and tests
    const char*        display;        // human-readable name for logs and the node's info
    const char*        logTag;         // short bracket tag for per-cook log lines, e.g. [FFTW3]
    const char* const* dlls;           // candidate file names, most specific first
    int                dllCount;       // entries in dlls; build it with dllCountOf() below
    // False when the library ignores FFTW's plan rigour flags. The engine still accepts a policy
    // parameter, but reports that the library is not honouring it rather than pretending.
    bool               honoursPolicy;
    // The version string this backend's library is expected to report, or nullptr when the exact
    // version is not pinned (MKL ships under its own version scheme).
    const char*        expectedVersion;
    // True for oneMKL. oneMKL ships three interchangeable threading layers and defaults to the Intel
    // OpenMP one, which means loading mkl_rt pulls libiomp5md.dll into the process. TouchDesigner
    // already ships and loads its own libiomp5md.dll, and two Intel OpenMP runtimes in one process is
    // the documented "Error #15: Initializing libiomp5md.dll, but found libiomp5md.dll already
    // initialized" abort, plus thread oversubscription. Intel's own guidance is to link a single
    // OpenMP runtime per process, and the ISSL forbids modifying or working around the DLL itself.
    // So the only fix available to a plugin is to make oneMKL load its sequential layer instead of
    // its OpenMP one, which is what this flag does at load time: one loaded OpenMP runtime (the
    // host's), no oversubscription, and transforms that stay on the calling thread - which is also
    // exactly what the node's Async-off contract asks for. False for FFTW3, which has no layers.
    bool               forceSequentialThreading;
    const char*        note;           // one line on what this backend is and is not good for
};

// The vendored source build is named with its version (see 3rdParty/fftw3/VERSION); the older
// unversioned names are accepted so an existing install keeps working.
//
// The unversioned name says nothing about what is behind it. fftw.org's prebuilt Windows DLL has
// that name and is 3.3.5/SSE2, but a `libfftw3f-3.dll` built by this project from a 3.3.10 tree is
// also out there and reports "fftw-3.3.10-sse2-avx-avx2-avx2_128" - measured, on the copy in
// __Plugins__/FFT. So this list is a preference order, not a claim: whichever name matches first is
// loaded and *reports* what it is, and backendVersionMatches() turns a surprise into a warning in
// the plan line instead of into a silently different set of timings.
inline const char* const kFftw3Dlls[] = {
    "libfftw3f-3.3.11-avx2.dll",       // vendored, self-built, AVX2 + FMA
    "libfftw3f-3.dll",                 // unversioned: fftw.org 3.3.5/SSE2, or any older source build
    "fftw3f.dll",                      // a plain FFTW source build
};

// oneMKL ships the FFTW3 interface inside the single dynamic library, so the wrapper library
// described in Intel's "Building the FFTW3 interface" notes is not needed: verified by reading the
// export table of the real mkl_rt.3.dll (oneMKL 2026.1.0), which has 95 fftwf_* exports including
// every symbol resolved below. The file name carries the ABI suffix Intel bumps between releases:
// mkl_rt.3.dll for oneMKL 2026.x, mkl_rt.2.dll for 2025.x, unversioned mkl_rt.dll for 2020.4 and
// earlier. All three are listed oldest-name-last so whichever the machine has is found.
inline const char* const kMklDlls[] = {
    "mkl_rt.3.dll",
    "mkl_rt.2.dll",
    "mkl_rt.dll",
};

// How many candidate names one of the arrays above holds. A function over the array rather than a
// number typed into the descriptor, so adding a file name cannot leave the count behind - a stale
// count is silent, and it either hides a fallback or reads past the end of the array.
template <size_t N>
constexpr int dllCountOf(const char* const (&)[N]) { return static_cast<int>(N); }

// constexpr rather than merely const: Parameters.cpp asserts the menu value <-> registry mapping at
// compile time, and reading a descriptor inside a static_assert needs the object to be usable in a
// constant expression. Everything in FftBackendInfo is a literal type, so nothing is lost.
inline constexpr FftBackendInfo kFftw3Backend = {
    "fftw3",
    "FFTW3 (vendored AVX2 build)",
    "FFTW3",
    kFftw3Dlls, dllCountOf(kFftw3Dlls),
    /* honoursPolicy   */ true,
    /* expectedVersion */ "3.3.11",
    /* forceSequentialThreading */ false,   // FFTW3 has no threading layers
    "portable: runs on Intel and AMD, and honours the FFT planner policy and wisdom cache",
};

inline constexpr FftBackendInfo kMklBackend = {
    "mkl",
    "Intel oneMKL (FFTW3 interface)",
    "oneMKL",
    kMklDlls, dllCountOf(kMklDlls),
    /* honoursPolicy   */ false,
    /* expectedVersion */ nullptr,
    /* forceSequentialThreading */ true,    // keeps libiomp5md.dll out of TouchDesigner's process
    // Verified against the oneMKL docs and the real DLL: the plan rigour flags, FFTW_WISDOM_ONLY and
    // the wisdom save/load functions are all accepted and ignored, and planning itself is a few
    // hundred microseconds regardless of flag - so the Planner Policy menu and the wisdom cache are
    // both inert here, which is why honoursPolicy is false.
    "Intel-tuned kernels; chooses its own plan, so the FFT planner policy and wisdom do not apply",
};

// Backend 0 is the default: the vendored build, which is the one this repository ships and tests.
inline const FftBackendInfo& defaultBackend() { return kFftw3Backend; }

// Registry, indexed by the menu value in Parameters.h. Kept as a function rather than a table so a
// third library can be added by extending the switch and kMaxBackends, with the compiler naming
// every place that has to change. Two entries today is why the UI is a toggle and not a menu.
// constexpr so the menu-value contract can be asserted at compile time where the menu is built:
// see the static_assert in Parameters.cpp.
inline constexpr int backendCount() { return 2; }

inline constexpr const FftBackendInfo& backendById(int index)
{
    switch (index) {
        case 1:  return kMklBackend;
        case 0:
        default: return kFftw3Backend;
    }
}

// Reverse of backendById(), for code that needs a dense per-backend slot (a latch, a counter) rather
// than a descriptor. Unknown descriptors land on the default's slot, which is the safe direction: a
// descriptor the registry does not know about cannot be selected, so nothing can collide with it.
inline int backendIndex(const FftBackendInfo& info)
{
    for (int i = 0; i < backendCount(); ++i)
        if (&backendById(i) == &info)
            return i;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// 3. Loading
// ---------------------------------------------------------------------------------------------
struct FftBackend {
    FftApi                 api;
    const FftBackendInfo*  info   = nullptr;
    void*                  module = nullptr;   // HMODULE once loaded
    std::string            path;               // resolved file name, for the log
    std::string            error;              // why it could not be loaded, if it could not

    bool loaded() const { return module != nullptr && api.complete(); }
    const char* name() const { return info ? info->display : "<no backend>"; }
};

#ifdef _WIN32

// Directory of the module this code was compiled into, which is where the FFT DLLs are deployed
// beside FFT.dll. An address inside the module identifies it without depending on its file name,
// so a renamed or relocated plugin still finds its own directory. The address of a function-local
// static is used rather than a function's own address because linkers may fold identical functions
// together, and a folded copy could belong to a different module.
inline std::string pluginDirectory()
{
    static const int anchor = 0;
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&anchor), &self))
        return std::string();
    char path[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(self, path, MAX_PATH))
        return std::string();
    std::string s(path);
    const size_t cut = s.find_last_of("\\/");
    return (cut == std::string::npos) ? std::string() : s.substr(0, cut);
}

// Loads one library and fills in the table. Returns false with a reason in out.error when the file
// is absent or does not export the required entry points; either way the caller falls through to
// the next candidate rather than failing the plugin.
inline bool loadBackendFrom(const FftBackendInfo& info, const std::string& dir,
                            const char* dllName, FftBackend& out)
{
    const std::string full = dir.empty() ? std::string(dllName) : (dir + "\\" + dllName);

    // LOAD_WITH_ALTERED_SEARCH_PATH so the library's own dependencies resolve next to it rather
    // than against the host executable's directory - TouchDesigner's install folder, in practice.
    HMODULE h = LoadLibraryExA(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h && !dir.empty())
        h = LoadLibraryExA(dllName, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // fall back to PATH
    if (!h) {
        out.error = "not found: " + full;
        return false;
    }

    auto sym = [h](const char* n) -> void* {
        return reinterpret_cast<void*>(GetProcAddress(h, n));
    };

    FftApi api;
    api.alloc        = reinterpret_cast<decltype(api.alloc)>(sym("fftwf_malloc"));
    api.dealloc      = reinterpret_cast<decltype(api.dealloc)>(sym("fftwf_free"));
    api.planR2C      = reinterpret_cast<decltype(api.planR2C)>(sym("fftwf_plan_dft_r2c_1d"));
    api.executeR2C   = reinterpret_cast<decltype(api.executeR2C)>(sym("fftwf_execute_dft_r2c"));
    api.destroyPlan  = reinterpret_cast<decltype(api.destroyPlan)>(sym("fftwf_destroy_plan"));
    api.importWisdom = reinterpret_cast<decltype(api.importWisdom)>(sym("fftwf_import_wisdom_from_filename"));
    api.exportWisdom = reinterpret_cast<decltype(api.exportWisdom)>(sym("fftwf_export_wisdom_to_filename"));
    api.sprintPlan   = reinterpret_cast<decltype(api.sprintPlan)>(sym("fftwf_sprint_plan"));
    api.initThreads  = reinterpret_cast<decltype(api.initThreads)>(sym("fftwf_init_threads"));
    api.planWithNthreads = reinterpret_cast<decltype(api.planWithNthreads)>(sym("fftwf_plan_with_nthreads"));
    api.version      = reinterpret_cast<const char*>(sym("fftwf_version"));
    api.compiler     = reinterpret_cast<const char*>(sym("fftwf_cc"));

    // oneMKL: pick the sequential threading layer before the library takes its first real call, so it
    // never loads mkl_intel_thread + libiomp5md.dll. See FftBackendInfo::forceSequentialThreading for
    // why (the host already has an Intel OpenMP runtime loaded and two in one process aborts).
    // MKL_THREADING_SEQUENTIAL == 1, from mkl_service.h: MKL_THREADING_INTEL 0, SEQUENTIAL 1, GNU 3,
    // TBB 4. The call has to happen here rather than lazily: oneMKL reads its threading layer on the
    // first library call, and the first thing this plugin does with the library is allocate buffers.
    // A build without the symbol is not an error - the plugin then simply gets oneMKL's default
    // layer, which is what earlier versions did.
    if (info.forceSequentialThreading) {
        api.setThreadingLayer = reinterpret_cast<decltype(api.setThreadingLayer)>(sym("MKL_Set_Threading_Layer"));
        if (api.setThreadingLayer) api.setThreadingLayer(1 /* MKL_THREADING_SEQUENTIAL */);
    }

    if (!api.complete()) {
        // Name the missing entry points. "Found the DLL but it is not an FFTW3 library" is a
        // different problem from "the DLL is not there", and this is the only place that can tell
        // them apart - in particular it is what distinguishes a plain mkl_rt.dll that does not
        // carry the FFTW3 interface from one that does.
        std::string missing;
        if (!api.alloc)       missing += " fftwf_malloc";
        if (!api.dealloc)     missing += " fftwf_free";
        if (!api.planR2C)     missing += " fftwf_plan_dft_r2c_1d";
        if (!api.executeR2C)  missing += " fftwf_execute_dft_r2c";
        if (!api.destroyPlan) missing += " fftwf_destroy_plan";
        FreeLibrary(h);
        out.error = "loaded " + full + " but it does not export the FFTW3 C interface:" + missing;
        return false;
    }

    out.module = h;
    out.api    = api;
    out.path   = full;
    out.error.clear();
    return true;
}

// Tries each candidate file name in turn, remembering every failure so the caller can log a single
// line that names all the places that were looked in.
inline bool loadBackend(const FftBackendInfo& info, FftBackend& out,
                        const std::string& dir = std::string())
{
    out = FftBackend();
    out.info = &info;
    const std::string searchDir = dir.empty() ? pluginDirectory() : dir;

    std::string tried;
    for (int i = 0; i < info.dllCount; ++i) {
        if (loadBackendFrom(info, searchDir, info.dlls[i], out))
            return true;
        tried += (tried.empty() ? "" : "; ") + out.error;
    }
    out.error = "no usable " + std::string(info.display) + " library. Tried: " + tried;
    return false;
}

#else   // !_WIN32 - keep the engine buildable off Windows (tests, CI) with a null backend

inline std::string pluginDirectory() { return std::string(); }
inline bool loadBackend(const FftBackendInfo& info, FftBackend& out,
                        const std::string& = std::string())
{
    out = FftBackend();
    out.info = &info;
    out.error = "dynamic backend loading is only implemented on Windows";
    return false;
}

#endif  // _WIN32

// ---------------------------------------------------------------------------------------------
// 3b. Process-wide cache
// ---------------------------------------------------------------------------------------------
// Loading is done once per library per process and the result is kept for the lifetime of the
// process. Deliberately, a backend is never unloaded - not even when the user toggles away from it.
//
// FreeLibrary on either of these libraries is a way to crash the host. Both keep internal state
// bound to the loading thread and spawn their own worker threads; a plan or a worker that outlives
// the unload faults at an address inside a module that is no longer mapped, and because TouchDesigner
// hosts the plugin, the crash lands in the user's project rather than in this code. Residency is the
// cheaper side of that trade, and a second FFT CHOP node reuses the same loaded library instead of
// mapping its own copy.
//
// The cache is keyed by FftBackendInfo::id, so any descriptor with the same id resolves to the same
// loaded module. Callers are expected to pass the canonical descriptors declared above.
inline constexpr int kMaxBackends = 4;   // raise when a third library is offered in the menu

struct BackendCache {
    const FftBackendInfo* key[kMaxBackends] = {};
    FftBackend            value[kMaxBackends];
    int                   count = 0;
};

inline BackendCache& backendCache()
{
    static BackendCache cache;
    return cache;
}

// For tests that need to re-resolve, e.g. after swapping the file on disk. Drops the records but
// does not unload anything - see above.
inline void resetBackendCache()
{
    backendCache() = BackendCache();
}

inline const FftBackend& cachedBackend(const FftBackendInfo& info)
{
    BackendCache& cache = backendCache();
    for (int i = 0; i < cache.count; ++i) {
        if (cache.key[i]->id && info.id && std::strcmp(cache.key[i]->id, info.id) == 0)
            return cache.value[i];
    }
    if (cache.count < kMaxBackends) {
        cache.key[cache.count] = &info;
        loadBackend(info, cache.value[cache.count]);
        ++cache.count;
        return cache.value[cache.count - 1];
    }
    // More distinct registries than slots; load without caching rather than hand back nothing.
    static FftBackend overflow;
    loadBackend(info, overflow);
    return overflow;
}

// ---------------------------------------------------------------------------------------------
// 4. Reporting
// ---------------------------------------------------------------------------------------------
// Which SIMD kernels the library chose for a plan. Both FFTW and MKL write codelet names into the
// plan text, so scanning it answers the question from the library's own account rather than from an
// inference about the file on disk. The name field is documented "for debugging only", but it is
// populated and it is what makes an AVX2 build distinguishable from an SSE2 one at run time: the
// two produce identical results and accept identical calls.
//
// The returned buffer is released with plain free(), NOT with the backend's dealloc. This is not a
// style choice: FFTW's manual (doc/reference.texi, "Reversing the Plan") says the string from
// fftw_sprint_plan is "a newly allocated NUL-terminated string (which the caller is responsible for
// deallocating with free)", and the implementation agrees - api/print-plan.c uses plain malloc while
// fftwf_free lands in kernel/kalloc.c, where the MSVC branch defines real_free as _aligned_free.
// Handing a malloc'd pointer to _aligned_free reads a bogus header word in front of the block and
// corrupts the heap, which aborts the host with STATUS_HEAP_CORRUPTION. oneMKL's FFTW3 interface
// mirrors FFTW's documented contract, so free() is correct for both backends.
inline std::string sprintPlanText(const FftApi& api, fftwf_plan plan)
{
    if (!plan || !api.sprintPlan)
        return std::string();
    char* raw = api.sprintPlan(plan);
    const std::string text = raw ? std::string(raw) : std::string();
    if (raw)
        std::free(raw);   // see above - free(), never api.dealloc
    return text;
}

inline std::string describePlanSimd(const FftApi& api, fftwf_plan plan)
{
    if (!plan || !api.sprintPlan)
        return "plan kernels: not reported by this backend";
    const std::string text = sprintPlanText(api, plan);
    if (text.empty())
        return "plan kernels: not reported by this backend";

    const char* family = nullptr;
    if      (text.find("_avx2") != std::string::npos) family = "AVX2 (256-bit vectors, FMA-capable codelets)";
    else if (text.find("_avx")  != std::string::npos) family = "AVX (128-bit vectors for single precision)";
    else if (text.find("_sse2") != std::string::npos) family = "SSE2 (128-bit)";
    return std::string("plan kernels: ") + (family ? family : "none named in the plan");
}

// The first line of the plan that actually names a codelet, for the log. The plan is a tree whose
// first line is only the top-level node, so printing that shows nothing of interest.
inline std::string firstCodeletLine(const FftApi& api, fftwf_plan plan)
{
    if (!plan || !api.sprintPlan)
        return std::string();
    const std::string text = sprintPlanText(api, plan);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.find("_avx2") != std::string::npos || line.find("_avx") != std::string::npos ||
            line.find("_sse2") != std::string::npos)
            return line;
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return std::string();
}

// The version the loaded library reports, with FFTW's "fftw-<version>-<simd>" wrapper stripped, or
// the raw string when it does not follow that shape (oneMKL reports its own versioning).
inline std::string reportedVersion(const FftBackend& backend)
{
    const char* v = backend.api.version;
    if (!v) return std::string();
    if (std::strncmp(v, "fftw-", 5) != 0) return std::string(v);
    const std::string s(v);
    const size_t dash = s.find('-', 5);
    return s.substr(5, dash == std::string::npos ? std::string::npos : dash - 5);
}

// True when the loaded library is the one this plugin was built and measured against. A backend
// with no pinned version (oneMKL) always passes: there is nothing to be wrong about.
inline bool backendVersionMatches(const FftBackend& backend)
{
    const char* want = backend.info ? backend.info->expectedVersion : nullptr;
    if (!want || !backend.api.version) return true;
    return reportedVersion(backend) == want;
}

// One line stating which library is live and whether it is the version the plugin was built and
// measured against. Call this once per plan, never per cook.
inline std::string describeBackend(const FftBackend& backend)
{
    if (!backend.loaded())
        return std::string("no FFT library - ") + backend.error;

    std::string s = backend.api.version ? backend.api.version : "<no version string>";
    s += " [";
    s += backend.path;
    // "wisdom on" is only honest when the library does something with it. oneMKL exports both
    // functions and ignores them, so on that backend the answer is "none" - see importWisdomOnce.
    if (!backend.info || backend.info->honoursPolicy)
        s += backend.api.hasWisdom() ? ", wisdom on" : ", no wisdom";
    else
        s += ", no wisdom cache";
    if (backend.api.initThreads && backend.api.planWithNthreads)
        s += ", threads API present (unused)";
    // Which threading layer oneMKL loaded, because it decides whether the host's OpenMP runtime is
    // shared or a second copy is pulled in. "sequential" is the plugin's own choice, made at load.
    if (backend.info && backend.info->forceSequentialThreading)
        s += backend.api.setThreadingLayer ? ", sequential threading layer" : ", default threading layer";
    s += "]";

    const char* want = backend.info ? backend.info->expectedVersion : nullptr;
    if (want && !backendVersionMatches(backend)) {
        s += " ** MISMATCH: this plugin is pinned to FFTW " + std::string(want) +
             " but the loaded library reports " + reportedVersion(backend) +
             " - wisdom will be re-measured and timings will differ from the documented ones";
    }
    // Stated explicitly rather than left implicit, because the planner dropdown stays visible and
    // interactive on a backend that ignores it.
    if (backend.info && !backend.info->honoursPolicy)
        s += " - note: this library picks its own plan, so the FFT Planner policy has no effect";
    return s;
}

} // namespace FFTDSP

#endif // TD_PLUGIN_HAS_FFTW3

#endif // FFT_BACKEND_H
