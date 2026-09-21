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

How to read this file
---------------------
This header has four parts, in the order you would use them:

  1. The ABI subset       - struct FftApi: the table of function pointers every backend must fill in.
  2. Backend descriptors  - struct FftBackendInfo and the two static descriptors that describe one
                            library each, plus the small registry (defaultBackend / backendCount /
                            backendById / backendIndex) that turns the Backend parameter's integer
                            into one of those descriptors.
  3. Loading              - pluginDirectory / loadBackendFrom / loadBackend: the DLL search and the
                            GetProcAddress calls that populate an FftApi.
  3b. Process-wide cache  - backendCache / cachedBackend: one load per library per process, and no
                            unload, ever.
  4. Reporting            - the strings the log, the Info CHOP and the plan row show: which library
                            is live, which SIMD kernels it chose, and what version it reports.

The whole path from "the node wants FFTW" to "the entry points are resolved" is four calls:

  the Backend parameter is an int  ->  backendById() picks a descriptor  ->  cachedBackend() returns
  the process-wide FftBackend for that descriptor, loading it the first time  ->  loadBackend() walks
  the descriptor's dlls[] list  ->  loadBackendFrom() does one LoadLibraryExA plus a GetProcAddress per
  symbol, filling in an FftApi.

After that, every engine call goes through backend.api (the function-pointer table) and never names a
library symbol directly. That indirection is the whole mechanism, and section 1 is where it lives.
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
//
// WHAT: this struct is the plugin's entire view of an FFT library. Nothing in the plugin calls a
// library symbol by name; every transform, allocation and plan destroy goes through one of the
// pointers below, set once by loadBackendFrom().
// WHY: the two candidate libraries export identical names, so only one can be linked in. Pointers
// resolved at run time are the way to have either (see the top of this file).
// HOW TO CHANGE: every member here has a matching GetProcAddress line in loadBackendFrom() and, for
// the required five, a term in complete(). Adding a member means adding both. Never mix members from
// two different libraries at run time: a plan made by one library's planR2C must be destroyed by that
// same library's destroyPlan, and its buffers freed by that same library's dealloc - a cross-library
// free is heap corruption, not a leak (see the malloc/free note above sprintPlanText).
// USED BY: filled in by loadBackendFrom(); read by FFTDSP::FFTWEngine in DSPModules.h, which is what
// the tests (tests/dsp_tests.cpp) and the bench (bench/bench.cpp) drive.
struct FftApi {
    // ---- functions the plugin needs; a backend is unusable unless all of these resolve ----
    // WHICH FIVE ARE REQUIRED IS DEFINED BY complete() BELOW: if you add a required pointer, add it
    // there too, or a library missing it will be accepted and fail on the first transform.
    //
    // alloc / dealloc: the library's OWN aligned allocator pair (fftwf_malloc / fftwf_free). Buffers
    // handed to planR2C must come from here - the library's alignment requirement is not the
    // standard allocator's - and they must be released with the matching dealloc, never free().
    void* (*alloc)(size_t)                             = nullptr;  // fftwf_malloc
    void  (*dealloc)(void*)                            = nullptr;  // fftwf_free
    // planR2C: builds the real-to-complex 1-D plan for N floats in, N/2+1 complex out. The last
    // argument is the FFTW rigour flag set; a backend with honoursPolicy false ignores it.
    fftwf_plan (*planR2C)(int, float*, fftwf_complex*, unsigned) = nullptr;  // fftwf_plan_dft_r2c_1d
    // executeR2C: runs an already-built plan on already-allocated buffers. This is the call that runs
    // per cook; it must not allocate, and (per FFTW's manual) it is the only thread-safe entry point.
    void  (*executeR2C)(const fftwf_plan, float*, fftwf_complex*) = nullptr; // fftwf_execute_dft_r2c
    // destroyPlan: releases a plan. Null-check what you pass it against the plan being non-null; the
    // plugin checks both, because a library that failed to plan is still a library you must not call
    // a stale destroy on.
    void  (*destroyPlan)(fftwf_plan)                   = nullptr;  // fftwf_destroy_plan

    // ---- optional; present only if the library supports wisdom ----
    // MKL's FFTW3 interface accepts these calls but has no wisdom to save; it is reported rather
    // than assumed so the log does not claim a plan was cached when it was not.
    //
    // Wisdom is FFTW's saved record of how a plan was measured, so a MEASURE-grade plan does not have
    // to be re-measured on the next run. importWisdom loads a saved file, exportWisdom writes one.
    // hasWisdom() below requires BOTH, because one without the other only half works - see its note.
    int   (*importWisdom)(const char*)                 = nullptr;  // fftwf_import_wisdom_from_filename
    int   (*exportWisdom)(const char*)                 = nullptr;  // fftwf_export_wisdom_to_filename
    // sprintPlan: renders a plan to a heap string, which is the only runtime way to see which SIMD
    // kernels the library picked. Optional, and used only for reporting - section 4.
    char* (*sprintPlan)(const fftwf_plan)              = nullptr;  // fftwf_sprint_plan

    // ---- optional; the plugin does not plan in parallel, but the version report states whether
    // the API exists, because it changes what the library can do ----
    // These two are resolved for their EXISTENCE only. The plugin never calls them: the node's own
    // threading contract is one analysis thread, and FFTW cannot split a single 1-D transform
    // usefully anyway. describeBackend() prints "threads API present (unused)" when they are there.
    int   (*initThreads)(void)                         = nullptr;  // fftwf_init_threads
    void  (*planWithNthreads)(int)                     = nullptr;  // fftwf_plan_with_nthreads
    // Not an fftwf_ symbol at all: oneMKL's own switch for which threading layer the library loads.
    // Resolved here because it is the only way to keep oneMKL out of the host's OpenMP runtime - see
    // FftBackendInfo::forceSequentialThreading for why that matters. nullptr on FFTW3, which has no
    // such concept (fftwf_init_threads / plan_with_nthreads are its knobs instead).
    //
    // Only resolved when the descriptor asks for it, and it MUST be called during load, before the
    // library does anything else - loadBackendFrom() is where that happens, and moving it later is
    // the one change that silently reintroduces the libiomp5md abort.
    int   (*setThreadingLayer)(int)                    = nullptr;  // MKL_Set_Threading_Layer

    // ---- data symbols, not functions ----
    // Resolved by address. For a data export GetProcAddress returns the address of the array, so
    // these can be read directly as C strings. This is not a convenience: it is the whole reason the
    // FFTW_DLL / dllimport trap described at the top of this file cannot recur here.
    const char* version  = nullptr;   // fftwf_version, e.g. "fftw-3.3.11-sse2-avx-avx2-avx2_128"
    const char* compiler = nullptr;   // fftwf_cc, the compile command line
    // compiler is resolved but never read anywhere in the plugin; it is kept so the table mirrors the
    // library's advertised surface, and a future diagnostic can print it without touching this file.

    // Deliberately absent, and this comment is the reason: fftwf_cleanup() and
    // fftwf_forget_wisdom() free state that is *process-global* - every plan, every cached
    // trigonometric table, the whole wisdom set. There is no such thing as cleaning up "this node's"
    // FFTW state, so calling either from a plugin that can be instanced would invalidate the plans
    // other FFT CHOPs are executing with, and the manual's "the only thread-safe routine is
    // fftw_execute" rule means the damage would land in whichever cook thread ran next. A plan is
    // released with destroyPlan() and wisdom is simply left in memory; nothing here needs cleanup.
    // They are not resolved above on purpose, so there is no way to call them by accident.

    // A backend without any of the required five cannot run a transform at all.
    // WHAT: the single gate that decides whether a loaded DLL may be used. An api that fails this is
    // reported as "found the DLL but it is not an FFTW3 library" rather than used.
    // HOW TO CHANGE: only add a term if the engine cannot run without that symbol. Every term added
    // here makes a library unusable that previously worked, so this list is deliberately the minimum.
    // CALLED BY: loadBackendFrom() (before the module is kept) and FftBackend::loaded(), which is what
    // FFTWEngine::prepare() in DSPModules.h checks before planning.
    bool complete() const {
        return alloc && dealloc && planR2C && executeR2C && destroyPlan;
    }
    // Wisdom is optional, but the two halves must agree: importing without exporting would make
    // every MEASURE plan cost full price on every run of the host application.
    // WHAT: true when the library can both load and save a wisdom file. Requires BOTH.
    // HOW TO CHANGE: do not relax this to "either" - a library that only imports looks like it has a
    // wisdom cache while never writing one, and every run re-measures from scratch.
    // CALLED BY: importWisdomOnce() and exportWisdom() in DSPModules.h, and describeBackend() below.
    bool hasWisdom() const { return importWisdom && exportWisdom; }
};

// ---------------------------------------------------------------------------------------------
// 2. Backend descriptors
// ---------------------------------------------------------------------------------------------
// WHAT: one of these describes one FFT library - what to call it, which files to look for, and which
// features it does and does not have.
// WHY: the code needs to know about a library before it is loaded (to search for it) and after (to
// report on it), and the answer is the same either way. Keeping it in static data means one place to
// edit per library.
// HOW TO CHANGE: everything here is read-only by convention. The two descriptors below are declared
// constexpr because Parameters.cpp asserts against them in static_asserts; keep any new one
// constexpr too, or those asserts stop compiling. "How to add a third backend" at the end of this
// section lists every switch and list that must gain an entry.
// READ BY: loadBackendFrom()/loadBackend() (section 3), cachedBackend() (3b) which keys on id,
// FFTWEngine::prepare() in DSPModules.h (which picks and reports the library), backendIndex() below,
// and Parameters.cpp (the menu-value static_asserts).
struct FftBackendInfo {
    // id: the identity the rest of the code keys on. It is a short lowercase word, not a file name
    // and not a display name. cachedBackend() compares ids to decide "same library, already loaded",
    // so two descriptors that mean the same library must carry the same id, and two that do not must
    // never share one - sharing an id would silently hand back the wrong loaded module.
    const char*        id;             // stable key, used by parameters and tests
    // display: what a human reads in the log, the Info DAT and the popup. Free to change; nothing
    // parses it.
    const char*        display;        // human-readable name for logs and the node's info
    // logTag: the short bracket prefix on plan log lines, e.g. "[FFTW3] plan N=16384 ...". Kept
    // separate from display so log lines stay narrow, and so two backends are distinguishable at a
    // glance in a log that spans a backend switch.
    const char*        logTag;         // short bracket tag for per-cook log lines, e.g. [FFTW3]
    // dlls + dllCount: the ordered search list, most preferred first. Order matters - it is a
    // preference, not a set, and loadBackend() stops at the first name that resolves. dllCount must
    // equal the number of entries; never type it by hand, call dllCountOf().
    const char* const* dlls;           // candidate file names, most specific first
    int                dllCount;       // entries in dlls; build it with dllCountOf() below
    // False when the library ignores FFTW's plan rigour flags. The engine still accepts a policy
    // parameter, but reports that the library is not honouring it rather than pretending.
    //
    // PLAINLY: true means the Fast/Auto/Measured/Patient menu really changes how the library plans.
    // False means it does not, and the plugin says so in the log and in the Info DAT instead of
    // leaving a control on screen that does nothing. Setting it true for a library that ignores the
    // flags is a lie the user acts on; setting it false for one that honours them costs only a
    // slightly less useful status string. When in doubt, false is the safe direction.
    // READ BY: FFTWEngine::prepare() (which skips wisdom and the background measure when false),
    // describeBackend(), and the static_assert in Parameters.cpp that pins backend 0 to true.
    bool               honoursPolicy;
    // The version string this backend's library is expected to report, or nullptr when the exact
    // version is not pinned (MKL ships under its own version scheme).
    // PLAINLY: this is what turns "the DLL on this machine is not the one we measured" from a silent
    // difference in timings into a visible MISMATCH line. nullptr means "no claim" and therefore
    // never a mismatch - correct for oneMKL, whose versions do not match FFTW's numbering.
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
    //
    // PLAINLY: true makes loadBackendFrom() call MKL_Set_Threading_Layer(sequential) right after the
    // library is opened, before anything else touches it. That call must stay where it is: oneMKL
    // latches its threading layer on its first real call, and the plugin's first real call is an
    // allocation. Moving it later, or making it conditional on a machine that "looks like it has no
    // OpenMP", reintroduces a hard abort inside TouchDesigner. Do not set this true for a library
    // that has no MKL_Set_Threading_Layer export - the call is simply skipped, and the flag would
    // then only affect the wording of the status string.
    bool               forceSequentialThreading;
    // note: the one-line "what this is good for" shown in the Info DAT and the popup. Free text.
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
//
// WHAT: these two arrays are the search order for each backend, consulted by loadBackend() one name
// at a time until one opens. WHY a list and not one name: the correct file name depends on what the
// user has installed, and a plugin that only accepts one exact spelling refuses to work on machines
// that are fine. HOW TO CHANGE: add a name at the END unless it should win over the ones already
// there - order is preference, and putting a generic name first is what makes a machine with several
// installs pick the wrong one. The count is not written down anywhere; it is dllCountOf() over the
// array, so adding a line cannot leave a stale count behind.
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
//
// Same contract as kFftw3Dlls above. Note the search order is newest first on purpose: the ABI
// suffix is bumped between oneMKL releases, and pointing at the newest name found is what makes an
// upgraded oneMKL get used without an entry being reordered.
inline const char* const kMklDlls[] = {
    "mkl_rt.3.dll",
    "mkl_rt.2.dll",
    "mkl_rt.dll",
};

// How many candidate names one of the arrays above holds. A function over the array rather than a
// number typed into the descriptor, so adding a file name cannot leave the count behind - a stale
// count is silent, and it either hides a fallback or reads past the end of the array.
//
// WHAT: counts the entries of a fixed-size char* array at compile time. WHY: so no descriptor ever
// states a number a human has to keep in sync. HOW TO CHANGE: nothing to change - it adapts to the
// array. It only accepts a real array (a reference to one), so it cannot be called on a pointer,
// which is what stops it from being used somewhere its answer would be meaningless.
// CALLED BY: the two descriptor initialisers below, for dlls/dllCount.
template <size_t N>
constexpr int dllCountOf(const char* const (&)[N]) { return static_cast<int>(N); }

// constexpr rather than merely const: Parameters.cpp asserts the menu value <-> registry mapping at
// compile time, and reading a descriptor inside a static_assert needs the object to be usable in a
// constant expression. Everything in FftBackendInfo is a literal type, so nothing is lost.
//
// The two descriptors below differ in exactly four ways, and each one is deliberate:
//   honoursPolicy             - true for FFTW3 (the rigour flags work), false for oneMKL.
//   expectedVersion           - "3.3.11" for the vendored build, nullptr for oneMKL (no version
//                               numbering to pin against).
//   forceSequentialThreading  - false for FFTW3, true for oneMKL (see the field comment above).
//   the dlls list and the three display strings.
// Everything else about them is the same shape, which is what makes "how to add a third backend"
// below a short list rather than a redesign.
// The default descriptor, index 0, and the library this repository ships, tests and measures against.
// The other one is below.
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

// Index 1. The interesting differences from kFftw3Backend are honoursPolicy false (the plugin tells
// the user the Planner Policy menu does nothing here) and forceSequentialThreading true (the load
// path reaches for MKL_Set_Threading_Layer before the library's first real call).
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
//
// WHAT: the descriptor to use when nothing has been chosen, or when a chosen one cannot be loaded.
// WHY this one: it is the only library guaranteed to be beside the plugin, and it is the one every
// documented number was measured on. HOW TO CHANGE: do not repoint this at a backend that may not be
// installed - FFTWEngine::prepare() in DSPModules.h falls back HERE when the selected library is
// missing, and a default that is itself optional turns one missing DLL into an unusable node.
// CALLED BY: FFTWEngine::prepare(), wisdomPathFor() and importWisdomOnce()/exportWisdom() in
// DSPModules.h; AnalysisPipeline::backendInfo() (AnalysisPipeline.h); and FFT::getInfoDATEntries()
// in FFT.cpp, for the fft_backend Info DAT row.
inline const FftBackendInfo& defaultBackend() { return kFftw3Backend; }

// Registry, indexed by the menu value in Parameters.h. Kept as a function rather than a table so a
// third library can be added by extending the switch and kMaxBackends, with the compiler naming
// every place that has to change. Two entries today is why the UI is a toggle and not a menu.
// constexpr so the menu-value contract can be asserted at compile time where the menu is built:
// see the static_assert in Parameters.cpp.
//
// WHAT: how many backends the plugin offers. WHY it is a function and not a constant: it is read in
// static_asserts, which need a constant expression. HOW TO CHANGE: this is one of the numbers that
// must move with Parameters::Backend::COUNT - the two are asserted equal in Parameters.cpp, so they
// cannot drift silently.
// CALLED BY: Parameters.cpp (the static_assert above the menu builder), backendIndex() below,
// tests/dsp_tests.cpp (which loops over every backend, and asserts the count is at least 2), and
// bench/bench.cpp indirectly through backendById(). Nothing in DSPModules.h calls it directly.
inline constexpr int backendCount() { return 2; }

// WHAT: turns the Backend parameter's integer into a descriptor. This is the one place the menu
// value and the library are tied together.
// WHY a switch: it is constexpr, which lets Parameters.cpp assert the mapping at compile time, and
// it makes the compiler point at the omission if a value is ever added to the enum and not here.
// The default arm is deliberately backend 0 rather than an error: an out-of-range value can only
// come from a corrupt parameter file, and cooking with the shipped library is better than failing.
// HOW TO CHANGE: add a case for the new index and leave 0 as the default arm. Every case must be a
// distinct descriptor - two cases returning the same one would make backendIndex() ambiguous.
// CALLED BY: AnalysisPipeline::rebuild() and its rebuild-needed check (source/AnalysisPipeline.cpp);
// the two static_asserts in Parameters.cpp; backendIndex() below; tests/dsp_tests.cpp; and
// bench/bench.cpp. DSPModules.h does not call it - the engine receives a descriptor pointer.
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
//
// WHAT: descriptor -> small integer, for indexing per-backend arrays.
// HOW TO CHANGE: it compares ADDRESSES, so it only recognises the descriptors backendById() returns.
// A copy of a descriptor, or a locally built one, answers 0 - correct as a fallback, wrong if you
// were expecting a distinct slot. Keep passing the canonical descriptors.
// CALLED BY: FFTWEngine::importWisdomOnce()/exportWisdom() in DSPModules.h, which index static
// once_flag/ok arrays of kMaxBackends slots by this answer.
inline int backendIndex(const FftBackendInfo& info)
{
    for (int i = 0; i < backendCount(); ++i)
        if (&backendById(i) == &info)
            return i;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// How to add a third backend
// ---------------------------------------------------------------------------------------------
// Everything that must gain an entry, verified against the code. The first two are compile-checked
// by the static_asserts in Parameters.cpp, so they cannot be forgotten; the rest are not.
//
//   1. FftBackend.h (here): add a dlls[] array and a constexpr FftBackendInfo for the library, set
//      backendCount() to 3, and add the new case to backendById(). kMaxBackends (section 3b) is 4
//      today, so it already has room - raise it only if you go past four.
//   2. source/Parameters.h: add the value to enum class Backend and set COUNT to 3.
//   3. source/Parameters.cpp: make the Backend control a real menu, not a toggle - the control is
//      built today by appendToggle(..., FftbackendName, ...) in Parameters::setup(), and three
//      states do not fit a toggle. Then extend the getI(FftbackendName) mapping in
//      Parameters::eval(), which currently reads "anything non-zero means oneMKL". The
//      static_asserts above that code will fail until COUNT and the new endpoint agree.
//   4. Deploy the DLL next to FFT.dll (the plugin's own directory). Section 3 explains why that is
//      where the search starts.
//
// Nothing else is per-backend: wisdom files are already named per backend id by
// FFTWEngine::wisdomPathFor() in DSPModules.h, and the per-backend telemetry slots are sized by
// backendIndex()/kMaxBackends.
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// 3. Loading
// ---------------------------------------------------------------------------------------------
// WHAT: one loaded (or failed-to-load) library: the resolved entry points, which descriptor it came
// from, the HMODULE, and a reason string when it did not work.
// WHY the error lives here and not in an exception: a failed load is a normal outcome the node
// reports and cooks through (see the top of this file), not an error to unwind.
// HOW TO CHANGE: adding a field is free. Do not add a destructor that calls FreeLibrary - see
// section 3b for why nothing here ever unloads.
// USED BY: cachedBackend() below returns one by reference; FFTWEngine holds one as m_backend
// (DSPModules.h) and reads api/info/path/error from it; describeBackend() in section 4 reports on it.
struct FftBackend {
    FftApi                 api;
    const FftBackendInfo*  info   = nullptr;
    void*                  module = nullptr;   // HMODULE once loaded
    std::string            path;               // resolved file name, for the log
    std::string            error;              // why it could not be loaded, if it could not

    // WHAT: "this backend can be used" - the module opened AND every required entry point resolved.
    // Both halves matter: a load that opened a DLL which is not an FFTW3 library leaves module set
    // and api incomplete, and that must read as not loaded.
    // CALLED BY: FFTWEngine::prepare() in DSPModules.h (its "no library" branch) and
    // describeBackend() below.
    bool loaded() const { return module != nullptr && api.complete(); }
    // WHAT: the human-readable name, with a placeholder when info was never set - which is the state
    // of a default-constructed FftBackend.
    // CALLED BY: FFTWEngine::prepare() in DSPModules.h, in the log line that says which library was
    // loaded and from where.
    const char* name() const { return info ? info->display : "<no backend>"; }
};

#ifdef _WIN32

// Directory of the module this code was compiled into, which is where the FFT DLLs are deployed
// beside FFT.dll. An address inside the module identifies it without depending on its file name,
// so a renamed or relocated plugin still finds its own directory. The address of a function-local
// static is used rather than a function's own address because linkers may fold identical functions
// together, and a folded copy could belong to a different module.
//
// WHAT: returns the folder the plugin binary lives in, as a string, or "" if Windows will not say.
// WHY the trick above: GetModuleHandleExA with FROM_ADDRESS needs an address that is certainly inside
// this module. `&anchor` is the address of a file-static variable declared inside this function, so
// it is guaranteed to be in this module and cannot be merged with anything (variables are not folded
// the way functions are). That is the whole reason a variable is used instead of, say, &pluginDirectory.
// WHY not the process's own path: TouchDesigner's executable directory is not where a plugin's DLLs
// live, and GetModuleFileNameA with a null handle would hand back exactly that.
// HOW TO CHANGE: nothing here should need to change. Do not add caching of the answer in a way that
// outlives the module, and do not switch to a path derived from the host executable - that is the
// one behaviour this function exists to avoid. The "" return is meaningful: loadBackend() then hands
// the bare DLL name to Windows, which searches PATH, and that is the documented fallback.
// CALLED BY: loadBackend() below, when its caller passed no directory.
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
//
// WHAT: one attempt at one file name. Opens it, resolves every symbol in FftApi, and either hands
// back a usable FftBackend or a reason string.
// WHY it returns false instead of throwing or aborting: an FFT library is optional equipment. The
// node must keep working without one, so every failure here is data, not control flow.
// HOW TO CHANGE: the three things to keep in step are (a) a new FftApi member needs a GetProcAddress
// line here, (b) the "not found" and "not an FFTW3 library" cases must stay DISTINCT - the missing
// list below is what tells a plain mkl_rt.dll apart from one with the FFTW3 interface, and (c) the
// sequential-threading call must stay before the completeness check and before anything else calls
// into the library (see FftBackendInfo::forceSequentialThreading).
// CALLED BY: loadBackend() below, once per candidate file name.
inline bool loadBackendFrom(const FftBackendInfo& info, const std::string& dir,
                            const char* dllName, FftBackend& out)
{
    const std::string full = dir.empty() ? std::string(dllName) : (dir + "\\" + dllName);

    // LOAD_WITH_ALTERED_SEARCH_PATH so the library's own dependencies resolve next to it rather
    // than against the host executable's directory - TouchDesigner's install folder, in practice.
    // The practical consequence for MKL: opened this way it finds its own mkl_intel_thread /
    // mkl_tbb_thread DLLs beside itself instead of hunting through TouchDesigner's folder.
    HMODULE h = LoadLibraryExA(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h && !dir.empty())
        h = LoadLibraryExA(dllName, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // fall back to PATH
    if (!h) {
        out.error = "not found: " + full;
        return false;
    }

    // One lookup helper, so every entry below is a single line that states the FFTW name it resolves.
    // GetProcAddress returns nullptr for a symbol the library does not export; each member is then
    // reinterpret_cast to its declared signature, which the FftApi declaration check makes type-safe.
    auto sym = [h](const char* n) -> void* {
        return reinterpret_cast<void*>(GetProcAddress(h, n));
    };

    // The order below matches FftApi. Adding a member there means adding a line here; a typo in a
    // name string is not a compile error, it is a nullptr - which for a required symbol shows up as
    // this library being rejected, and for an optional one as a feature quietly reported as absent.
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
        //
        // Keep this list in step with complete(): every symbol that makes a backend "incomplete"
        // should also be named here, or the log will say a library is unusable without saying why.
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

// WHAT: the "try every candidate name" loop - one call that means "load this backend", however many
// files that takes.
// WHY it collects `tried`: the user gets one log line naming every file that was looked for, which
// is the difference between "the plugin found nothing" and "here is where to put the DLL".
// HOW TO CHANGE: the default argument is what makes the directory optional; passing "" is the same
// as omitting it, and both mean pluginDirectory(). out is reset at the top, so an FftBackend handed
// in with stale contents cannot be mistaken for a successful load - keep that reset if you ever
// reorder anything here.
// CALLED BY: cachedBackend() below, and tests/dsp_tests.cpp exercises the descriptors through the
// engine rather than calling this directly.
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

// WHAT: the off-Windows stand-ins. pluginDirectory() says "no directory" and loadBackend() reports
// that it cannot do the job, so the engine builds and runs its "no FFT library" path instead of
// refusing to compile. WHY: the DSP engine, the tests and the bench are platform-independent; only
// the DLL search is Windows-specific, so only it is stubbed.
// HOW TO CHANGE: if a second platform ever needs real loading, this is the block to extend - and the
// stub must keep returning false with a reason, because the engine treats a load failure as normal.
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
//
// PLAINLY, WHY NOTHING IS EVER UNLOADED: a FreeLibrary here would run a library's shutdown code
// while TouchDesigner may still hold a plan or a worker thread from it, and the fault lands inside
// TouchDesigner's process, which is the user's project. Keeping the library mapped costs a few MB
// and cannot crash anything. This is a deliberate trade, not an oversight - do not "fix" the leak.
// There is no counter of users and no unload path to get wrong.
//
// WHAT: how many distinct backends the cache can hold at once - a fixed-size array, not a limit on
// how many backends can exist in the registry.
// HOW TO CHANGE: raise it only when backendCount() exceeds it. Too small is not fatal (cachedBackend
// falls back to a shared static) but it means repeated loads; too large just wastes a few bytes.
inline constexpr int kMaxBackends = 4;   // raise when a third library is offered in the menu

// WHAT: the process-wide "descriptor -> loaded library" table.
// WHY an array of descriptors and not a std::map keyed by string: the number of backends is tiny and
// fixed, so a linear scan beats any hashing, and the array needs no allocation on the load path.
// There is one instance, inside backendCache() below - do not make another, or two parts of the
// plugin could end up holding two different HMODULEs for the same library.
struct BackendCache {
    const FftBackendInfo* key[kMaxBackends] = {};
    FftBackend            value[kMaxBackends];
    int                   count = 0;
};

// WHAT: the single BackendCache, created on first use.
// WHY a function-local static rather than a global: it is initialised on first call, in whatever
// order the plugin's own startup happens to run, and it avoids any static-initialisation-order
// question between translation units.
// CALLED BY: cachedBackend() and resetBackendCache() below, nowhere else.
inline BackendCache& backendCache()
{
    static BackendCache cache;
    return cache;
}

// For tests that need to re-resolve, e.g. after swapping the file on disk. Drops the records but
// does not unload anything - see above.
//
// Note for a reader: this is currently not called from the plugin, tests/dsp_tests.cpp or
// bench/bench.cpp - nothing in the repository references it today. It is kept as the documented way
// to force a re-resolve, and the comment above states what it does and does not do.
inline void resetBackendCache()
{
    backendCache() = BackendCache();
}

// WHAT: "give me the loaded library for this descriptor", loading it the first time and handing the
// same one back on every later call. This is the only correct way to get an FftBackend in the plugin.
// WHY keyed on id rather than on the pointer: two descriptors that mean the same library should
// share one load, and id is the field that says they do. WHY the overflow fallback at the end: a
// full cache must never turn into "no library available" - an uncached extra load is the safe
// failure, and it cannot happen with the two backends the registry has today.
// HOW TO CHANGE: the returned reference lives in the cache and is never invalidated (nothing ever
// unloads or overwrites an occupied slot), so callers may hold it. If you ever add eviction, that
// guarantee goes away and FFTWEngine::m_backend would be the first thing to break.
// CALLED BY: FFTWEngine::prepare() in DSPModules.h (twice - once for the requested backend, once for
// the default when the requested one failed to load).
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
// WHAT: every function here turns loaded-library facts into a string for a human. None of them
// affects the transform, and nothing in the engine branches on their results - if one of these
// returned "" the plugin would compute exactly the same numbers and only read worse.
// WHY they live here rather than in the engine: they need only an FftApi and an FftBackend, so
// keeping them in this header keeps the vendor-library knowledge in one file.
// HOW TO CHANGE: free to edit the wording. Do not move the malloc/free note below into the code as a
// simplification - see it before changing anything in sprintPlanText().
//
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
//
// WHAT: runs the library's plan-to-string function and copies the result into a std::string, so the
// FFTW-allocated buffer never escapes this file. Returns "" for a null plan or a backend without
// sprintPlan (both are normal, not errors).
// HOW TO CHANGE: the free() below is the one line here that must not be "improved" into
// api.dealloc - the comment above is why. Everything else is a plain copy.
// CALLED BY: describePlanSimd() and firstCodeletLine() below. Nothing else in the plugin or in
// tests/dsp_tests.cpp calls it, and bench/bench.cpp does not either.
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

// WHAT: one line naming the SIMD family the plan's codelets are built for (AVX2 / AVX / SSE2), or a
// sentence saying this backend does not report one.
// WHY it matters at all: two builds that produce identical numbers can differ several-fold in speed,
// and the plan text is the only run-time source that names the difference.
// HOW TO CHANGE: the three substrings are the library's own codelet naming, so extend the list only
// for a family the libraries actually name this way. The AVX2 test must stay FIRST - "_avx" is a
// substring of "_avx2", so reordering these makes every AVX2 build report as plain AVX.
// CALLED BY: FFTWEngine::prepare() in DSPModules.h, in the once-per-plan log line.
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
//
// WHAT: walks the plan text line by line and returns the first line mentioning a SIMD codelet, or "".
// WHY: it is the raw material describePlanSimd() summarises - the summary says "AVX2", this says
// which AVX2 codelet.
// Note: this function is currently not called from the plugin, tests/dsp_tests.cpp or
// bench/bench.cpp - nothing in the repository references it today. It is kept as available detail
// for the log, and if you wire it up, feed it the same api the plan was made with.
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
//
// WHAT: the bare version number, e.g. "3.3.11" out of "fftw-3.3.11-sse2-avx-avx2-avx2_128".
// HOW TO CHANGE: the split is on the dash AFTER position 5, which is what keeps "fftw-" itself from
// being treated as the separator. The parsing is deliberately tolerant - an unrecognised shape comes
// back whole rather than empty - because the only consumer is a comparison and a log line.
// CALLED BY: backendVersionMatches() and describeBackend() below. Nothing else.
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
//
// WHAT: the "is my timing table still valid?" question, answered as a boolean.
// HOW TO CHANGE: the two early-outs are the whole contract - no expected version, or a library that
// reports none, both mean "cannot be wrong" and must keep returning true. A false result changes a
// log line only; it never stops the plugin from transforming.
// CALLED BY: describeBackend() below, and FFTWEngine::prepare() in DSPModules.h, which passes the
// answer to PlanLog::log() as a flag.
inline bool backendVersionMatches(const FftBackend& backend)
{
    const char* want = backend.info ? backend.info->expectedVersion : nullptr;
    if (!want || !backend.api.version) return true;
    return reportedVersion(backend) == want;
}

// One line stating which library is live and whether it is the version the plugin was built and
// measured against. Call this once per plan, never per cook.
//
// WHAT: the single "what am I running on?" string: version, file path, wisdom state, whether the
// threads API exists, which threading layer oneMKL chose, and a MISMATCH warning when the library is
// not the pinned one.
// WHY once per plan: it builds several strings and reads the plan, which is cheap but not free, and
// none of it changes between cooks. CALLING IT PER COOK would put a handful of allocations on the
// real-time path for information that has not changed.
// HOW TO CHANGE: wording is free. The MISMATCH branch and the "policy has no effect" note are the
// two that carry a promise to the user - if a new backend has different caveats, this is where they
// belong, so the node never presents a control that does nothing without saying so.
// CALLED BY: FFTWEngine::backendReport() in DSPModules.h (which FFT.cpp then reads for the Info DAT
// and popup) and FFTWEngine::prepare() itself, once per plan.
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
