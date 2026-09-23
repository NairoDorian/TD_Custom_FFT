#ifndef DSP_MODULES_H
#define DSP_MODULES_H

/*
===========================================================================
                        HIGH-PERFORMANCE DSP ENGINE
===========================================================================
Module Architecture: Real-Time Audio Signal Processing & Psychoacoustic FFT
Targeted Platform: TouchDesigner Custom CHOP C++ Plugin (x86_64 AVX2)

This header is TouchDesigner-independent: it is shared by the plugin, the
headless unit tests (tests/dsp_tests.cpp) and the benchmark (bench/bench.cpp).

Processing Pipeline Overview:
1. Audio Ingestion: Circular Ring Buffering (FIFOBuffer) - zero heap allocations.
2. Pre-Filtering: Parametric Biquad Equalizer (BiquadEQ / Direct Form II Transposed).
3. Tapering & Padding: Window Generator (Kaiser/Hann/...) with coherent-gain or full-scale normalization.
4. Spectral Analysis: Single-Precision FFTW3 R2C Transform (FFTWEngine, wisdom-cached plans).
5. SIMD Vectorization: 256-bit AVX2 magnitude spectrum (2x unrolled, rsqrt).
6. Psychoacoustic Re-mapping: Log/Mel/ERB/Bark/Chroma/Melog with identity bypass (AVX2 gather).
7. Equal-Loudness Weighting: A / C / ITU-R 468 (2x unrolled).
8. Dynamic Range Conversion: dB with selectable reference, single-gather 2048-entry
   mantissa-LUT log10 (AVX2) - no interpolation, which measured 2x slower for no gain.
9. Temporal Smoothing: Asymmetric attack/release envelope (2x unrolled FMA).
10. Real-time plumbing: wait-free TripleBuffer handoff + WorkerSignal wake-up. The handoff
    and the wake-up are mutex-free on the cook thread; a plan swap or a deferred log still
    takes a lock, but only when one of those rare events actually occurred.

SIMD alignment policy
---------------------
AVX2 code uses ALIGNED loads/stores only where alignment is guaranteed by construction
(buffer base pointers from AlignedAllocator, offsets that are multiples of 8 floats).
Everywhere else the unaligned variants are used. MSVC emits vmovups for both, so this
is a correctness/portability guarantee (Clang/GCC would fault on a misaligned vmovaps),
not a performance change.

How to read this file
---------------------
The sections are numbered, and those numbers are cited from other files and from the
README, so treat "section 6c" as a stable address, not as decoration:

    0    AlignedAllocator            everything AVX2 in here allocates through this
    0a   DenormalGuard               FTZ/DAZ around any code that runs IIR or envelopes
    0b   cpuSupportsAVX2()           the guard that keeps the node from faulting on an old CPU
    0c   PlanLog + python_logger     where every log line in the plugin ends up
    0d   TripleBuffer                the two lock-free cook <-> worker handoffs
    0e   WorkerSignal                the worker's sleep/wake primitive
    1    FIFOBuffer                  the per-channel ring buffer at the head of the pipeline
    2    BiquadEQ                    the optional shelf EQ, applied at ingest
    3    WindowGenerator             the taper applied to each analysis window
    4    PerceptualWarping           the index-to-Hz remapping (Log/Mel/ERB/...) + its tables
    5    EqualLoudness              A / C / ITU-R 468 weighting curves
    6a   FastLog10                   the mantissa-LUT 20*log10
    6b   SIMD helpers                multiplyInto / multiplyInPlace / peak search
    6c   DecibelConverter            magnitude -> dB, and dB -> normalized [0,1]
    7    BallisticsFilter            the attack/release envelope follower
    8    IFFTEngine                  the FFT engine interface, and the AVX2 magnitude kernel
    9    FFTWEngine                  the one implementation: planning, wisdom, backend choice

Everything here is TouchDesigner-independent on purpose: the plugin, the unit tests
(tests/dsp_tests.cpp) and the benchmark (bench/bench.cpp) all include this header, so a
change here is compiled three times and must not drag in a TouchDesigner type.

Where a function is a stage of the per-channel analysis, its doc block says so and names
the stage; AnalysisPipeline::runChannel() in AnalysisPipeline.h is the one place that calls
them in order, and is the fastest way to see the pipeline end to end.

Building what this file belongs to: see README.md, "Building". Running the tests that pin
it: PluginProjects/FFT/tests/dsp_tests.cpp (the suite reports its own check count).
===========================================================================
*/

#include <vector>
#include <cmath>
#include <complex>
#include <algorithm>
#include <tuple>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>
#include <immintrin.h> // AVX2 & FMA SIMD Compiler Intrinsics
#include <cstdlib>    // _aligned_malloc / _aligned_free for 32-byte SIMD alignment

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#endif
#include <condition_variable>

#include <fftw3.h>         // FFTW3 types only (fftwf_plan, fftwf_complex); nothing is linked here
#include "FftBackend.h"    // which FFT library is loaded, how it is chosen, and how it reports itself

/*
===========================================================================
  0. 32-BYTE ALIGNED ALLOCATOR (for AVX2 SIMD load/store alignment)
===========================================================================
WHAT: A std::allocator that hands out memory aligned to `Align` (32 bytes by default), so that
      AlignedVector (below) can be used with the aligned AVX2 intrinsics (_mm256_load_ps).

WHY:  The default allocator guarantees only alignof(max_align_t), which is 16 bytes. Every AVX2
      kernel in this file that uses an aligned load would fault (not merely run slowly) on a
      16-byte-aligned buffer, so the type of the container has to carry the guarantee rather than
      each call site remembering to check.

HOW TO CHANGE: Nothing to tune here. If a future CPU needs 64-byte alignment, change the default
      of `Align` and every AlignedVector in the project moves with it - but the SIMD code that
      explicitly uses the 32-byte intrinsics would also have to be revisited, so do not treat this
      as a knob.

      The one non-obvious requirement is the `rebind` member and the templated converting
      constructor: std::vector's implementation asks an allocator for the allocator of a different
      element type (it allocates raw bytes internally), so a bespoke allocator without rebind is a
      compile error rather than a runtime problem.

      `construct` / `destroy` place and remove elements in already-allocated storage. They are the
      pre-C++17 form of the allocator traits protocol; std::allocator_traits supplies defaults if
      they are absent, so they are only here to keep the behaviour explicit.
*/
template<typename T, std::size_t Align = 32>
struct AlignedAllocator {
    using value_type = T;

    // Required by std::vector: "give me the allocator for a different element type". The Align is
    // propagated unchanged, so a rebind cannot silently lose the alignment guarantee.
    template<typename U> struct rebind { using other = AlignedAllocator<U, Align>; };

    AlignedAllocator() noexcept = default;
    // The converting constructor is also part of the protocol: an allocator must be constructible
    // from the rebound one, and this one is alignment-only so nothing needs to be copied over.
    template<typename U> AlignedAllocator(const AlignedAllocator<U, Align>&) noexcept {}

    T* allocate(std::size_t n) {
        // A zero-size allocation is legal to request and must not be passed to _aligned_malloc,
        // which treats a size of 0 as an error and sets errno.
        if (n == 0) return nullptr;
#ifdef _MSC_VER
        void* ptr = _aligned_malloc(n * sizeof(T), Align);
#else
        // MSVC's _aligned_malloc memory must be released with _aligned_free (see deallocate), so
        // the two branches cannot share a single allocator function - the pairing is per-platform.
        void* ptr = nullptr;
        if (posix_memalign(&ptr, Align, n * sizeof(T)) != 0) ptr = nullptr;
#endif
        // The standard requires allocation failure to be reported as an exception, not as null.
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }
    void deallocate(T* p, std::size_t) noexcept {
        if (!p) return;
#ifdef _MSC_VER
        _aligned_free(p);
#else
        free(p);
#endif
    }
    template<typename U, typename... Args>
    void construct(U* p, Args&&... args) { new (static_cast<void*>(p)) U(std::forward<Args>(args)...); }
    template<typename U>
    void destroy(U* p) noexcept { p->~U(); }
    // All instances of this allocator are interchangeable (it holds no state), so every allocator
    // of the same element type compares equal - which is what lets std::vector move storage
    // between containers without reallocating.
    bool operator==(const AlignedAllocator&) const noexcept { return true; }
    bool operator!=(const AlignedAllocator&) const noexcept { return false; }
};

namespace FFTDSP {

// The two container types the DSP core uses for sample data. Prefer these over std::vector<float>
// anywhere a buffer may be handed to an AVX2 kernel that uses aligned loads; a plain
// std::vector<float> is only 16-byte aligned and would fault there.
using AlignedVector = std::vector<float, AlignedAllocator<float, 32>>;
using AlignedComplexVector = std::vector<std::complex<float>, AlignedAllocator<std::complex<float>, 32>>;

// Both are the double-precision value of pi, in the two precisions this file works in; PI_D is the
// one the filter and window maths uses (it is computed in double even where the result is float).
// PI_F is the float form, kept for call sites that need a float constant in a __m256 broadcast.
// Neither is currently referenced outside this line - they are here so the trig helpers have one
// definition of pi to share rather than each spelling the literal out.
constexpr float PI_F = 3.14159265358979323846f;
constexpr double PI_D = 3.14159265358979323846;

// True when `p` is 32-byte aligned, i.e. safe to pass to an aligned AVX2 load. It exists for
// assertions and for the tests, not for hot code: a runtime branch here would cost more than the
// unaligned load it was protecting. Not currently called from the plugin, tests or bench.
inline bool isAligned32(const void* p) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) & 31u) == 0;
}

/*
===========================================================================
  0a. DENORMAL GUARD (FTZ / DAZ)
===========================================================================
IIR filters and envelope followers decay into denormal floats on silence; every
operation on a denormal costs ~100 cycles. Set flush-to-zero + denormals-are-zero
for the duration of a cook / worker job and restore the previous MXCSR afterwards.

WHAT: An RAII object: construct it at the top of a block, and the FPU's control register is
      restored to exactly what it was when the block ends (including on an exception).

HOW TO USE: One instance per analysis entry point - the pipeline owner constructs it once around
      a whole job, not once per channel and not per sample. Nobody else needs to remember the flag,
      which is the point of it being a type rather than a pair of calls.

HOW TO CHANGE: It is thread-local by nature (MXCSR is a per-thread register), so a new thread that
      does IIR work must construct its own guard - it does not inherit the setting reliably.
      The two bits are the x86 names: bit 15 FTZ flushes subnormal *results*, bit 6 DAZ treats
      subnormal *inputs* as zero. Both are needed: DAZ alone still lets a decaying IIR write
      denormals that the next iteration must read.
*/
struct DenormalGuard {
    unsigned int saved;
    DenormalGuard() noexcept : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }   // FTZ (bit 15) | DAZ (bit 6)
    ~DenormalGuard() noexcept { _mm_setcsr(saved); }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;
};

// True if every sample is exactly 0.0 / -0.0 (digital silence). ~0.05 us for 735 samples.
//
// WHY this exists instead of just running the pipeline: a fully silent input still costs a full
// FFT and a full warp, and TouchDesigner cooks the node continuously whether or not it has audio.
// Detecting silence lets the pipeline skip the analysis for that channel entirely - but only on the
// plain linear-magnitude path: dB modes still need the floor value written and ballistics still
// need to decay, so AnalysisPipeline::runChannel only takes the short-circuit when both are off.
//
// HOW it works: it ORs the *bit patterns* rather than comparing to 0.0f, so -0.0f counts as
// silence while any nonzero sample (including a denormal, which == 0.0f is false for anyway but
// which a `<` comparison could get wrong under FTZ) clears the accumulator. The AVX2 path
// therefore reads the values as integers with the sign bit masked off, and the scalar tail does
// the same with memcpy.
inline bool blockIsSilent(const float* x, size_t n) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    __m256i acc = _mm256_setzero_si256();
    // One early exit after the loop rather than a branch per vector: _mm256_testz_si256 is true
    // only when every lane of the accumulator is zero, i.e. every sample seen was 0.0 or -0.0.
    // The sign-bit mask is applied once, after the loop: (a & m) | (b & m) == (a | b) & m, so
    // masking every load was one AND per vector for nothing (measured 132 -> 91 ns at 3175 samples).
    for (; i + 7 < n; i += 8) {
        acc = _mm256_or_si256(acc, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i)));
    }
    acc = _mm256_and_si256(acc, _mm256_set1_epi32(0x7FFFFFFF));   // clear the sign bit: -0.0 is silence
    if (!_mm256_testz_si256(acc, acc)) return false;
#endif
    for (; i < n; ++i) {
        uint32_t bits; std::memcpy(&bits, x + i, 4);
        if (bits & 0x7FFFFFFFu) return false;
    }
    return true;
}

// dst[i] = (a[i] + b[i]) (unaligned ok)
// Used to mix input channels down to one analysis channel; unaligned is deliberate because the
// inputs come from TouchDesigner's own CHOP buffers, whose alignment the plugin does not control.
inline void addInto(const float* __restrict a, const float* __restrict b, float* __restrict dst, size_t n) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 7 < n; i += 8) _mm256_storeu_ps(dst + i, _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
#endif
    for (; i < n; ++i) dst[i] = a[i] + b[i];
}

// x[i] *= g, in place. Same unaligned contract as addInto.
inline void scaleInPlace(float* __restrict x, size_t n, float g) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    const __m256 vg = _mm256_set1_ps(g);
    for (; i + 7 < n; i += 8) _mm256_storeu_ps(x + i, _mm256_mul_ps(_mm256_loadu_ps(x + i), vg));
#endif
    for (; i < n; ++i) x[i] *= g;
}

/*
===========================================================================
  0b. CPU FEATURE CHECK
===========================================================================
The plugin is compiled with /arch:AVX2. On a CPU without AVX2 the loader
must refuse to process instead of dying with an illegal-instruction fault.

WHAT: Returns true when this CPU can execute every instruction the compiled kernels use, so the
      node can be told to stay idle instead of faulting the whole TouchDesigner process.

WHY it is not simply "is AVX2 present": the kernels also use FMA (every _mm256_fmadd_ps /
      _mm256_fnmadd_ps in this file), and an AVX2-only CPU without FMA would fault on those. The
      OS must additionally have enabled the YMM register state (XCR0), or every 256-bit instruction
      faults regardless of what the silicon supports - which is the case in some VMs and under
      some hypervisors. So all three are required, and the FMA check is not redundant with AVX2.

HOW TO CHANGE: If a kernel here starts using an instruction set that is not already covered
      (AVX-512, for example), add its check to this function *and* to the message the node reports —
      the caller surfaces this as an error string, so a new requirement must say so by name.

CALLED BY: FillCHOPPluginInfo() in FFT.cpp, once per process, when TouchDesigner registers the DLL.
      The result is cached in that file's g_cpuHasAVX2 and copied per instance into FFT::myCpuOk,
      which is what the node's error string, the `simd_avx2_active` Info channel and the popup's
      SIMD line all report. It is also called by the test suite, which prints the answer.
      Deliberately not called from a hot path: it is a CPUID query, not a bit test.
*/
inline bool cpuSupportsAVX2() noexcept {
#if defined(_WIN32)
    // PF_AVX2_INSTRUCTIONS_AVAILABLE == 40 (Windows 10+)
    // Preferred when available: it is the OS's own answer and already accounts for the
    // OS/hypervisor having enabled the register state, which is the part CPUID cannot see.
    if (IsProcessorFeaturePresent(40)) return true;
    // Fallback for systems where that query is unavailable. CPUID leaf 7 / sub-leaf 0, EBX bit 5
    // is AVX2; leaf 1 ECX bit 27 is OSXSAVE (the OS can save YMM state) and bit 12 is FMA.
    int info[4] = { 0, 0, 0, 0 };
    __cpuid(info, 0);
    if (info[0] < 7) return false;   // leaf 7 does not exist, so AVX2 cannot either
    __cpuidex(info, 7, 0);
    const bool avx2 = (info[1] & (1 << 5)) != 0;
    __cpuid(info, 1);
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool fma = (info[2] & (1 << 12)) != 0;
    if (!avx2 || !osxsave || !fma) return false;
    // OSXSAVE says the OS *can* save the state; XCR0 says it actually enabled XMM (bit 1) and
    // YMM (bit 2). Both bits must be set or the 256-bit kernels must not run.
    const unsigned long long xcr0 = _xgetbv(0);
    return (xcr0 & 0x6) == 0x6; // XMM + YMM state enabled by the OS
#else
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
}

/*
===========================================================================
  0c. TEXTPORT LOGGER (per-instance history + optional Python textport echo)
===========================================================================
printf() from a DLL goes to the OS stdout, which TouchDesigner's Textport does
not show. PySys_WriteStdout() writes straight to sys.stdout (the Textport) —
no script compilation, no string escaping problems. The CPython symbols are
resolved once from the python DLL already loaded in the TouchDesigner process.

WHAT: A log line has two destinations and they are independent. (1) The in-memory history, which
      the Info DAT's plan_log_* rows read - this always happens. (2) The TouchDesigner Textport,
      which is optional per line and is the part that has to go through Python.

WHY THE SPLIT: the Textport path calls into CPython, and a worker thread calling Python is a bug
      waiting to happen (it would need the GIL and could block the analysis). So anything on the
      worker thread logs with echo disabled, or logs normally while the log is in *deferred* mode,
      where the message is queued and the cook thread writes it later (see setDeferred /
      flushToTextport). "Deferred" is therefore the normal state, not an error state.

HOW TO CHANGE: Adding a log line is just log("..."). The rules that matter: never call
      python_logger::writeToTextport() from a thread other than the cooking one, and prefer
      log(msg, false) for anything that can repeat every cook (the Textport is a shared, visible
      surface, and a line per cook is noise the user cannot turn off).
*/
constexpr size_t kMaxPlanLogEntries = 256;

// Clips one plan-log line to at most maxChars characters, appending "..." to mark that it was trimmed.
//
// It exists for the middle-click popup, which is the only surface in this plugin whose *rendering* has been
// observed to depend on the length of what it is given (~1660 characters rendered, ~1760 came up empty; no
// cap is documented on OP_String::setString). A backend description embeds the absolute path of the FFT
// library and runs to ~240 characters, so three of them made the popup's total length move by hundreds of
// characters between one cook and the next - a length-dependent failure that reads as a random one. Clipping
// here is what makes that length a constant.
//
// Only the popup clips. The Info DAT's plan_log_* rows and the textport carry the line whole, because neither
// is a fixed-size surface and a clipped log line read as the log would be worse than a long popup.
inline std::string clipLine(const std::string& s, size_t maxChars)
{
    return s.size() <= maxChars ? s : s.substr(0, maxChars) + "...";
}

#ifdef _WIN32
namespace python_logger {

enum class GILState { Locked, Unlocked };
using EnsureFn  = GILState(*)(void);
using ReleaseFn = void(*)(GILState);
using WriteFn   = void(*)(const char*, ...);

// The three resolved CPython symbols, or `ok == false` when the host has no Python. Held as plain
// function pointers so nothing here is linked against a Python import library.
struct Api {
    EnsureFn  ensure = nullptr;
    ReleaseFn release = nullptr;
    WriteFn   write = nullptr;
    bool      ok = false;
};

// Resolves the three CPython entry points once per process, from whichever Python DLL
// TouchDesigner has already loaded into this process. `ok` is false when none of them resolved,
// which is the normal outcome for the tests and the bench (no Python in the process) - callers
// must treat that as "no Textport", never as an error.
inline const Api& api() {
    static const Api resolved = [] {
        Api a;
        // A preference order, not a requirement: the first name that is already loaded wins. The
        // version-specific names come first so the exact interpreter is used when it is there, and
        // python3.dll last because it is the version-agnostic forwarder. TouchDesigner's own
        // GetModuleHandle rather than a load: the plugin must never load a *second* Python into a
        // process that already has one, and must do nothing at all if TD has none loaded.
        const char* names[] = { "python311.dll", "python312.dll", "python313.dll", "python310.dll", "python3.dll" };
        HMODULE h = nullptr;
        for (const char* n : names) { h = GetModuleHandleA(n); if (h) break; }
        if (!h) return a;
        a.ensure  = reinterpret_cast<EnsureFn>(GetProcAddress(h, "PyGILState_Ensure"));
        a.release = reinterpret_cast<ReleaseFn>(GetProcAddress(h, "PyGILState_Release"));
        a.write   = reinterpret_cast<WriteFn>(GetProcAddress(h, "PySys_WriteStdout"));
        // All three or nothing: releasing a GIL state that was never acquired, or writing without
        // holding it, are both worse than staying silent.
        a.ok = a.ensure && a.release && a.write;
        return a;
    }();
    return resolved;
}

// Cooking thread only (see the section comment above for why). Takes the GIL around the write,
// because PySys_WriteStdout is a CPython call and TouchDesigner's own Python may be running.
inline void writeToTextport(const std::string& msg) {
    const Api& a = api();
    if (!a.ok) return;
    GILState g = a.ensure();
    // PySys_WriteStdout truncates at 1000 bytes; split long messages.
    // Carriage returns are rewritten to spaces so a stray \r cannot move the Textport cursor.
    std::string line = msg;
    for (char& c : line) if (c == '\r') c = ' ';
    size_t pos = 0;
    while (pos < line.size()) {
        std::string chunk = line.substr(pos, 900);   // 900, not 1000: leaves room for the newline
        a.write("%s%s", chunk.c_str(), (pos + 900 >= line.size()) ? "\n" : "");
        pos += 900;
    }
    a.release(g);
}

} // namespace python_logger
#endif

/*
WHAT: One node instance's log. Holds the history of every line it has logged, plus (in deferred
      mode) a queue of lines still to be echoed to the Textport.

WHY IT IS NOT JUST A std::vector<std::string>: the log is written from two threads - the pipeline
      owner logs plan events, the cook thread flushes and reads - so every accessor takes the
      mutex, and the one question the cook thread asks on every frame ("is there anything to
      flush?") is answered by a lock-free atomic flag instead, so the real-time path never waits on
      whoever is logging. The two questions the Info DAT asks about the history ("did it change?",
      "how many rows?") are likewise answered by a version counter rather than by copying.

MEMORY: bounded. The history is capped at kMaxPlanLogEntries; when it would exceed the cap, log()
      drops the OLDEST HALF and keeps the newer half. That is why callers cannot assume an entry
      index is stable across cooks, and why the row count has to be frozen per cook (see
      FFT::getInfoDATSize).

HOW TO CHANGE: log() is the only writer. If a new reader needs the history, add an accessor here
      rather than reaching for the members; if a new reader needs "has it changed since I last
      looked", use version(), not a comparison of snapshots.
*/
class PlanLog {
public:
    // deferred = true: messages are queued and written to the Textport by flushToTextport()
    // (call it from the cooking thread). Worker threads must never call into Python directly.
    // Set to true by the FFT operator's constructor, so this is the normal mode in TouchDesigner.
    void setDeferred(bool deferred) { std::lock_guard<std::mutex> lock(m_mutex); m_deferred = deferred; }

    // Appends a line to the history, and (echoToTextport) either writes it straight to the Textport
    // or queues it for the next flushToTextport(), depending on the deferred flag.
    // echoToTextport = false is for lines that should be in the history but are not worth printing
    // every cook - the Info DAT will still show them.
    // Safe to call from any thread.
    void log(const std::string& msg, bool echoToTextport = true) {
        bool deferred;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_history.size() >= kMaxPlanLogEntries) {
                m_history.erase(m_history.begin(), m_history.begin() + static_cast<std::ptrdiff_t>(m_history.size() / 2));
            }
            m_history.push_back(msg);
            // Bumped under the lock, so it is consistent with the history a caller would have read: a
            // reader that saw version V saw exactly this content. This is what lets the Info DAT avoid
            // re-copying the whole history on every cook just to discover that nothing changed.
            m_version.fetch_add(1, std::memory_order_release);
            deferred = m_deferred;
            if (echoToTextport && deferred) {
                m_pending.push_back(msg);
                m_hasPending.store(true, std::memory_order_release);
            }
        }
#ifdef _WIN32
        if (echoToTextport && !deferred) python_logger::writeToTextport(msg);
#else
        (void)echoToTextport;
#endif
    }

    // Lock-free check for the real-time caller: true only if flushToTextport() has work to do.
    bool hasPending() const noexcept { return m_hasPending.load(std::memory_order_acquire); }

    // Write queued messages to the Textport (cooking thread only). Returns the number written.
    size_t flushToTextport() {
        if (!hasPending()) return 0;
        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            pending.swap(m_pending);
            m_hasPending.store(false, std::memory_order_release);
        }
#ifdef _WIN32
        for (const auto& m : pending) python_logger::writeToTextport(m);
#endif
        return pending.size();
    }
    // A full copy of the history. Simple, and O(entries) with an allocation per line - so it is the
    // wrong choice for anything called repeatedly. Prefer entry(), snapshotTail() or version().
    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_history;
    }
    // Version of the history. Changes whenever snapshot() would return something different - a new entry,
    // or the half-history truncation log() performs when the buffer is full. A caller that caches a
    // snapshot can compare this instead of copying the history just to find out it is unchanged.
    uint64_t version() const noexcept { return m_version.load(std::memory_order_acquire); }

    // The last `n` entries, newest last, into `out` (cleared first). O(n), not O(size): the popup shows
    // the tail of the log on every cook, and snapshot() there copied all kMaxPlanLogEntries (256) strings
    // - with their allocations - to then use three of them.
    void snapshotTail(size_t n, std::vector<std::string>& out) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.clear();
        const size_t start = m_history.size() > n ? m_history.size() - n : 0;
        out.reserve(m_history.size() - start);
        out.insert(out.end(), m_history.begin() + static_cast<std::ptrdiff_t>(start), m_history.end());
    }
    // Single entry (used per Info DAT row so the whole history is not copied for every row)
    // Out of range returns an empty string rather than throwing: the caller is walking rows whose
    // count was decided earlier in the same cook, so an index past the end is possible by design.
    std::string entry(size_t i) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return i < m_history.size() ? m_history[i] : std::string();
    }
    // How many entries the history holds right now. Note this can change between two calls in the
    // same cook (the pipeline owner is logging concurrently), which is exactly why the Info DAT
    // freezes the count - see FFT::getInfoDATSize.
    size_t size() const { std::lock_guard<std::mutex> lock(m_mutex); return m_history.size(); }
    // Drops the history and bumps the version, so a reader caching the old content notices. Does not
    // touch the pending queue: lines already logged still reach the Textport.
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_history.clear();
        m_version.fetch_add(1, std::memory_order_release);
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_history;
    std::deque<std::string> m_pending;
    std::atomic<bool> m_hasPending{ false };
    // Mirrors the state of m_history for readers that only need "did it change?" (see version()).
    std::atomic<uint64_t> m_version{ 0 };
    bool m_deferred{ false };
};

/*
===========================================================================
  0d. WAIT-FREE SINGLE-PRODUCER / SINGLE-CONSUMER HANDOFF (triple buffer)
===========================================================================
Three slots rotate between the roles "being written" (back), "latest complete"
(mid) and "being read" (front). Publishing and acquiring are one atomic
exchange each: neither side ever blocks, spins or allocates, and the reader
always sees a complete, torn-free T. "Latest wins": if the consumer is slower
than the producer, intermediate results are simply overwritten.
Used for both directions of the cook <-> analysis worker exchange, so the
cook thread never takes a mutex the worker might be holding while descheduled.
*/
template <typename T>
class TripleBuffer {
public:
    // Using it, from the producer:
    //     buf.back() = value;            // fill the slot the producer owns
    //     const bool dropped = buf.publish();
    // and from the consumer:
    //     if (buf.acquire()) use(buf.front());
    //     else               reuseTheLastOne();
    // Exactly one thread may be the producer and exactly one the consumer. That is the whole
    // contract: there is no lock, so two producers (or two consumers) would corrupt the index
    // packing with no diagnostic. The FFT operator uses one instance per direction for this reason.
    TripleBuffer() = default;
    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;

    // ---- producer side ----
    // The slot the producer may write. Valid until the next publish(); the producer must not hold
    // onto it across a publish, because publish() reassigns which slot this is.
    T& back() noexcept { return m_slots[m_back]; }
    // Makes back() the latest complete slot. Returns true if the previously published slot
    // had NOT been acquired by the consumer yet (i.e. it was dropped) - telemetry, not an error:
    // "latest wins" means dropping an intermediate result is the designed behaviour.
    bool publish() noexcept {
        const uint32_t prev = m_mid.exchange(m_back | kDirty, std::memory_order_acq_rel);
        m_back = prev & kIndexMask;
        return (prev & kDirty) != 0;
    }

    // ---- consumer side ----
    // Returns true if a newer slot was acquired; front() then refers to it (and stays valid until the next acquire()).
    // Returns false when nothing was published since the last acquire, in which case front() is
    // still the previously acquired content and the caller is expected to reuse it (the operator's
    // "hold the previous spectrum" behaviour).
    bool acquire() noexcept {
        if ((m_mid.load(std::memory_order_acquire) & kDirty) == 0) return false;
        m_front = m_mid.exchange(m_front, std::memory_order_acq_rel) & kIndexMask;
        return true;
    }
    const T& front() const noexcept { return m_slots[m_front]; }
    T& front() noexcept { return m_slots[m_front]; }
    // Whether acquire() would report a new slot. A cheap pre-check for a caller that wants to look
    // before it leaps; never a substitute for acquire(), which is what actually claims the slot.
    // Not currently called by the plugin or the tests - acquire()'s return value covers their needs.
    bool hasNew() const noexcept { return (m_mid.load(std::memory_order_acquire) & kDirty) != 0; }

    // All three slots (setup only, when no other thread is running). This is how a producer
    // pre-allocates the buffers inside the slots before any handoff happens, so the steady state
    // allocates nothing - see FFT::startWorker.
    T& slot(size_t i) noexcept { return m_slots[i]; }
    static constexpr size_t kSlots = 3;

private:
    // m_mid packs two things into one atomic word so that publish() and acquire() are each a single
    // exchange, with no window in between where the other side could observe a torn state:
    //   bits 0-1  slot index of the latest complete slot (2 bits is exactly enough for 3 slots)
    //   bit  2    "a fresh slot has been published since the last acquire" flag
    // The exchange in publish() therefore both installs the new index and sets the flag atomically,
    // and the exchange in acquire() both claims the index and clears the flag.
    static constexpr uint32_t kIndexMask = 3u;   // bits 0-1: slot index
    static constexpr uint32_t kDirty     = 4u;   // bit 2: unacquired publication pending
    T m_slots[kSlots];
    // One cache line each: m_back is written only by the producer, m_front only by the consumer and
    // m_mid by both. Packed together they shared one line, so every publish invalidated the consumer's
    // copy of m_front and vice versa (false sharing between the cook thread and the worker).
    alignas(64) std::atomic<uint32_t> m_mid{ 1u };   // slot 1 is "clean" at start
    alignas(64) uint32_t m_back{ 2u };
    alignas(64) uint32_t m_front{ 0u };
};

/*
===========================================================================
  0e. WORKER WAKE-UP (no mutex on the signalling side)
===========================================================================
Windows: an auto-reset Event (signal/wait) plus a high-resolution waitable
timer (waitFor). Nothing on the signalling side takes a lock, so the cook thread
can never block behind a descheduled worker. Elsewhere: mutex + condvar.
Measured on the signalling thread (worker blocked in the kernel): 4-5 us median,
16-18 us p99 for every Win32 primitive (WaitOnAddress, SetEvent, semaphore,
condvar) - the cost is the kernel unblocking a thread, not the primitive. That
is why the operator's worker polls with waitFor() while jobs are flowing and only
needs signal() after it has gone dormant.
*/
#if defined(_WIN32) && !defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002   // Windows 10 1803+ SDKs define it
#endif

/*
WHAT: The one primitive the analysis worker blocks on. signal() wakes it; wait() blocks until it is
      woken; waitFor(ms) blocks until it is woken or the timeout expires, whichever comes first.

WHY TWO WAITS: the worker is woken by the cook thread, and waking a thread costs 4-5 us of kernel
      time on the *signalling* side - which is the cook thread, i.e. the one thread that must never
      be delayed. So while jobs are flowing the worker does not get woken at all: it polls with
      waitFor(2 ms) and picks up whatever the lock-free job slot holds. signal() is used only for
      the transition out of dormancy, where a 2 ms delay would actually be visible (see FFT.cpp's
      worker loop and myWorkerDormant).

SEMANTICS: a signal() issued while nobody waits is LATCHED (auto-reset event / flag) and consumed by
      the next wait - that is what makes the dormancy handshake safe - but several signals collapse
      into one: this is a wake-up, not a counter. The job slot is the source of truth for how much
      work there is, and this only says "look again".

HOW TO CHANGE: The two implementations (Win32 event + waitable timer, or mutex + condvar
      elsewhere) must keep the same semantics, and waitFor must return false on timeout rather
      than blocking indefinitely - the worker's poll loop depends on that to notice a stop request.
*/
class WorkerSignal {
public:
    WorkerSignal() noexcept {
#ifdef _WIN32
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset: one signal -> one wake, never lost
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        m_highRes = (m_timer != nullptr);
        if (!m_timer) m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);   // pre-1803 fallback
#endif
    }
    ~WorkerSignal() {
#ifdef _WIN32
        if (m_timer) { CancelWaitableTimer(m_timer); CloseHandle(m_timer); }
        if (m_event) CloseHandle(m_event);
#endif
    }
    WorkerSignal(const WorkerSignal&) = delete;
    WorkerSignal& operator=(const WorkerSignal&) = delete;

    // Windows: the poll timer is a high-resolution waitable timer, so it fires within ~0.5 ms of the
    // requested timeout regardless of the process/system timer resolution (a plain timed wait is
    // rounded to the 15.6 ms clock tick when nobody has called timeBeginPeriod, which would make the
    // worker miss frames).
    bool highResolutionTimer() const noexcept { return m_highRes; }

    // Wakes whichever of wait()/waitFor() is blocked. Callable from any thread; on Windows it takes
    // no lock at all, because this is the call the cook thread makes and it must not be able to
    // block behind a descheduled worker.
    void signal() noexcept {
#ifdef _WIN32
        if (m_event) SetEvent(m_event);
#else
        { std::lock_guard<std::mutex> lock(m_mutex); m_flag = true; }
        m_cv.notify_one();
#endif
    }
    // Blocks until signal() was called since the last wait() (the signal is consumed).
    // Blocks indefinitely - the caller has no way out, so use it only for a state the caller is
    // certain will be signalled, which in this project is exactly one: the worker's dormant state,
    // where any stop request or new job signals it. Everywhere else uses waitFor().
    void wait() noexcept {
#ifdef _WIN32
        if (m_event) WaitForSingleObject(m_event, INFINITE);
#else
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_flag; });
        m_flag = false;
#endif
    }
    // Like wait() but returns false after timeout_ms without a signal. Lets a worker poll a lock-free
    // queue at a fixed rate so the producer never has to pay for a kernel wake-up (~4-5 us + tails).
    bool waitFor(uint32_t timeout_ms) noexcept {
#ifdef _WIN32
        if (!m_event) {
            // CreateEventW failed (effectively never) — fall back to a real sleep so the worker's poll
            // loop does not degenerate into a busy spin consuming a core while TouchDesigner is idle.
            Sleep(timeout_ms);
            return false;
        }
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(timeout_ms) * 10000LL;   // relative, 100 ns units
        if (m_timer && SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
            HANDLE h[2] = { m_event, m_timer };                          // the event wins when both are set
            return WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0;
        }
        return WaitForSingleObject(m_event, timeout_ms) == WAIT_OBJECT_0;
#else
        std::unique_lock<std::mutex> lock(m_mutex);
        bool ok = m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return m_flag; });
        m_flag = false;
        return ok;
#endif
    }
private:
#ifdef _WIN32
    HANDLE m_event{ nullptr };
    HANDLE m_timer{ nullptr };
    bool m_highRes{ false };
#else
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_flag{ false };
    bool m_highRes{ false };
#endif
};

/*
===========================================================================
 1. CIRCULAR RING BUFFER (FIFOBuffer)
===========================================================================
WHAT: The history of the last `capacity` input samples of ONE channel, so that any window length up
      to `capacity` can be read out of it at any time. add() appends new samples (overwriting the
      oldest once full); get() produces a linear, oldest-first copy for the analysis to consume.

WHY IT IS NEEDED: TouchDesigner hands the node a block of audio per cook whose size is whatever
      arrived since the last cook, while the analysis needs a window that is fixed and usually much
      longer (72 ms is 3175 samples at 44.1 kHz, and a cook may deliver 735). So the sample history
      has to outlive the cook, and the window has to be readable as one contiguous buffer.

WHY NOT A std::deque / a growing vector: this is on the ingest path of every cook. add() and get()
      take no lock, allocate nothing after construction, and are two memcpys (add) / one or two
      memcpys plus a possible memset (get). resize() is the only allocating call, and it is called
      only when the window length changes.

THE LAYOUT:
    m_data[0 .. m_capacity-1]   the ring; data wraps from the end back to index 0
    m_idx                       the write cursor: the next byte to write is m_data[m_idx]
    m_filled                    how many of the m_capacity slots hold real audio; saturates at
                                m_capacity, so it cannot distinguish "full" from "overfull", which
                                is fine because past that point the oldest samples are gone anyway.
      m_idx is computed as `end % m_capacity` when a write does not wrap, and as `count - first`
      when it does; add() reads that carefully, because getting it wrong corrupts the timeline
      silently rather than failing loudly.

THE ONE CONTRACT THAT MATTERS TO CALLERS: get() right-aligns and zero-pads while the buffer is
      still filling, so the NEWEST sample is always at out[capacity-1] no matter how much audio has
      arrived so far. A freshly created node therefore produces a window of mostly zeros rather
      than garbage, and a full buffer and a partially filled one put "now" in the same place.
      This is also why the pipeline does not need to know whether the node just started.

HOW TO CHANGE: capacity comes from FFT::execute, which computes it from the window-length parameter
      via windowSamplesFrom() (RateModel.h), so the buffer is exactly as long as the window and the
      two cannot disagree. The default of 3175 is the default window (72 ms at 44.1 kHz), not a
      tuned number: do not change it here, change the parameter default if that is the intent.
*/
class FIFOBuffer {
public:
    explicit FIFOBuffer(size_t capacity = 3175) { resize(capacity); }

    // Sets the capacity, clearing the contents. Called from the ingest path whenever the window
    // length changes (which resets the analysis anyway), so the loss of history is deliberate.
    void resize(size_t capacity) {
        m_capacity = std::max<size_t>(1, capacity);   // 0 would make the modulo below divide by zero
        m_data.assign(m_capacity, 0.0f);
        m_idx = 0;
        m_filled = 0;
    }

    size_t capacity() const noexcept { return m_capacity; }
    size_t filled() const noexcept { return m_filled; }

    // Appends `count` samples from `signal`. If more samples arrive than the buffer holds, the
    // newest m_capacity of them are kept and the rest are discarded - there is nowhere else for
    // them to go, and a window can never be longer than m_capacity anyway.
    inline void add(const float* signal, size_t count) noexcept {
        if (count == 0 || !signal) return;
        if (count >= m_capacity) {
            std::memcpy(m_data.data(), signal + count - m_capacity, m_capacity * sizeof(float));
            m_idx = 0;
            m_filled = m_capacity;
            return;
        }
        size_t end = m_idx + count;
        if (end <= m_capacity) {
            // Does not wrap: one copy, and the cursor lands at end (which may equal m_capacity).
            std::memcpy(m_data.data() + m_idx, signal, count * sizeof(float));
            m_idx = end % m_capacity;
        } else {
            // Wraps: two copies, head then tail. `first` is how many fit before the end.
            size_t first = m_capacity - m_idx;
            std::memcpy(m_data.data() + m_idx, signal, first * sizeof(float));
            std::memcpy(m_data.data(), signal + first, (count - first) * sizeof(float));
            m_idx = count - first;
        }
        if (m_filled < m_capacity) m_filled = std::min(m_capacity, m_filled + count);
    }

    // Linearized copy of the buffer (oldest sample first); right-aligned & zero-padded while filling.
    // Unwraps the ring into `out`, which is resized to m_capacity if it is not already that size.
    inline void get(AlignedVector& out) const noexcept {
        if (out.size() != m_capacity) out.resize(m_capacity);
        if (m_filled < m_capacity) {
            std::memset(out.data(), 0, m_capacity * sizeof(float));
            if (m_filled > 0) {
                size_t start_dest = m_capacity - m_filled;
                if (m_idx >= m_filled) {
                    std::memcpy(out.data() + start_dest, m_data.data() + m_idx - m_filled, m_filled * sizeof(float));
                } else {
                    size_t part1 = m_filled - m_idx;
                    std::memcpy(out.data() + start_dest, m_data.data() + m_capacity - part1, part1 * sizeof(float));
                    std::memcpy(out.data() + start_dest + part1, m_data.data(), m_idx * sizeof(float));
                }
            }
            return;
        }
        if (m_idx == 0) {
            std::memcpy(out.data(), m_data.data(), m_capacity * sizeof(float));
        } else {
            std::memcpy(out.data(), m_data.data() + m_idx, (m_capacity - m_idx) * sizeof(float));
            std::memcpy(out.data() + (m_capacity - m_idx), m_data.data(), m_idx * sizeof(float));
        }
    }

private:
    size_t m_capacity{ 1 };
    std::vector<float> m_data;
    size_t m_idx{ 0 };
    size_t m_filled{ 0 };
};

/*
===========================================================================
 2. DIRECT FORM II TRANSPOSED BIQUAD EQUALIZER (RBJ shelves)
===========================================================================
WHAT: Two second-order IIR filters in series - a high shelf and a low shelf - with the coefficients
      recomputed only when a parameter changes. This is the node's optional EQ page.

WHY "DIRECT FORM II TRANSPOSED": it is the standard biquad topology for floating-point audio. It
      needs only two state variables (z1, z2) instead of four, and its round-off behaviour is better
      than the direct forms because the large intermediate sums are not stored. The recurrence below
      is the textbook one and should not be rewritten to look "simpler" - the other orderings are
      numerically worse for exactly the low-frequency, high-Q cases a shelf EQ is used for.

WHY RBJ: the coefficient formulas are Robert Bristow-Johnson's Audio EQ Cookbook shelves, which is
      what makes the cutoff/gain/Q parameters mean what a user expects from any other EQ.

HOW TO CHANGE: The coefficient design is in BiquadEQ::designShelf and the per-sample recurrence in
      BiquadSection::process. If you add a third filter (a peak/notch, say), it goes in series in
      both processBlockInPlace and processAudio - they must be kept in the same order, because the
      tests assert that the two paths produce the same samples.
*/
struct BiquadSection {
    // Coefficients with a0 already divided out (so a0 is implicitly 1). A shelf that is switched
    // off carries b0 = 1, b1 = b2 = a1 = a2 = 0, i.e. an identity filter.
    float b0{ 1.0f }, b1{ 0.0f }, b2{ 0.0f };
    float a1{ 0.0f }, a2{ 0.0f };
    // The two state variables. They are the filter's memory of past samples, which is why they must
    // be preserved across blocks and are only cleared by reset() - see processBlockInPlace.
    float z1{ 0.0f }, z2{ 0.0f };
    // False when this shelf is a no-op (gain below the 0.01 dB threshold that designShelf applies).
    // process() then returns its input untouched, which is what makes an all-off EQ cost nothing.
    bool active{ false };

    // One sample through the Direct Form II Transposed recurrence. Inline and branch-light: this is
    // called once per input sample, i.e. 44100 times a second per channel.
    inline float process(float x) noexcept {
        if (!active) return x;
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    // Clears the filter's memory (no click, no carry-over). Called when the EQ is switched on or
    // the sample rate changes, so a stale state cannot be applied to a different signal.
    void reset() noexcept { z1 = 0.0f; z2 = 0.0f; }
};

/*
WHAT: The two shelves, their coefficient cache, and the two ways to run audio through them.

WHY THE CACHE: designing a shelf costs four trigonometric calls, and the parameters only change when
      the user moves a slider - but the design was previously redone on every cook. m_design_key
      holds the five values the coefficients were built from, so a cook whose parameters did not
      change skips the design entirely. If you add a parameter that feeds the coefficients, it must
      be added to the key or the filter will silently ignore it.

WHY TWO PROCESSING ENTRY POINTS: they are the same filter, differing only in where they put the
      output and who calls them.
        processBlockInPlace(data, n, amount)  - in place, streaming. This is the one the plugin uses,
              from FFT::ingest, on the new samples only. Filtering each sample exactly once as it
              arrives is what keeps the IIR state continuous and costs one pass over the new audio.
              (The alternative - filtering the whole analysis window every cook - was measured at 4x
              the work and additionally restarts the filter from a stale state at each window start.)
        processAudio(in, amount, out)         - out of place, whole buffer. Not used by the plugin:
              it is the reference the tests compare the streaming path against, and it is what the
              bench measures the ingest path against.
*/
class BiquadEQ {
public:
    explicit BiquadEQ(double sampling_rate = 44100.0) : m_sample_rate(sampling_rate) {}

    // Changes the rate the coefficients are designed for, and invalidates the cache (they are
    // frequency-dependent, so every coefficient has to be rebuilt). Called from FFT::ingest.
    void setSampleRate(double sr) noexcept {
        if (m_sample_rate != sr) {
            m_sample_rate = sr;
            m_design_key = std::make_tuple(0.0, 0.0, 0.0, 0.0, 0.0);
        }
    }

    // Designs one RBJ shelf into `sec`. `is_high` selects which of the two shelf shapes is built
    // from the same formulas (that is the `s` sign below). A gain smaller than 0.01 dB is treated
    // as "off": the shelf is deactivated and its state cleared, so a user who wants no EQ gets
    // bit-exact passthrough rather than a filter that is nearly-but-not-quite transparent.
    void designShelf(bool is_high, double cutoff_hz, double gain_db, double q_factor, BiquadSection& sec) noexcept {
        if (std::abs(gain_db) < 0.01) { sec.active = false; sec.reset(); return; }
        cutoff_hz = std::clamp(cutoff_hz, 1.0, m_sample_rate * 0.49);
        double w0 = 2.0 * PI_D * cutoff_hz / m_sample_rate;
        double A = std::pow(10.0, gain_db / 40.0);
        double alpha = std::sin(w0) / (2.0 * std::max(0.01, q_factor));
        double cos_w0 = std::cos(w0);
        double sqrt_A = std::sqrt(A);
        double s = is_high ? 1.0 : -1.0;

        double b0 =  A * ((A + 1.0) + s * (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha);
        double b1 = -s * 2.0 * A * ((A - 1.0) + s * (A + 1.0) * cos_w0);
        double b2 =  A * ((A + 1.0) + s * (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha);
        double a0 = (A + 1.0) - s * (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha;
        double a1 =  s * 2.0 * ((A - 1.0) - s * (A + 1.0) * cos_w0);
        double a2 = (A + 1.0) - s * (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha;

        sec.b0 = static_cast<float>(b0 / a0);
        sec.b1 = static_cast<float>(b1 / a0);
        sec.b2 = static_cast<float>(b2 / a0);
        sec.a1 = static_cast<float>(a1 / a0);
        sec.a2 = static_cast<float>(a2 / a0);
        sec.active = true;
    }

    // Re-designs both shelves if any of the five shaping parameters changed, and answers the one
    // question the ingest path asks: "is there anything to apply?" - true only when at least one
    // shelf is active AND the blend amount is above zero. The caller uses that to skip the whole EQ
    // pass, so it must stay cheap; the tuple compare above does that.
    // Note the amount is deliberately NOT part of the cache key: it is an output blend, not a
    // coefficient, so moving it does not invalidate the design.
    bool updateAndCheckActive(double gain_db, double cutoff_hz, double low_gain_db, double low_cutoff_hz,
                              double q_factor, double amount) noexcept {
        std::tuple<double, double, double, double, double> key{ gain_db, cutoff_hz, low_gain_db, low_cutoff_hz, q_factor };
        if (m_design_key != key) {
            designShelf(true, cutoff_hz, gain_db, q_factor, m_high_shelf);
            designShelf(false, low_cutoff_hz, low_gain_db, q_factor, m_low_shelf);
            m_design_key = key;
        }
        return (m_high_shelf.active || m_low_shelf.active) && (amount > 0.0);
    }

    // Runs a whole buffer through the EQ, writing to a separate output. See the class comment for
    // which caller uses this (the tests and the bench, not the plugin).
    // The two shelves are in series, high then low, and `amount` is a dry/wet blend:
    // amount = 1 gives the filtered signal, amount = 0 gives the input back unchanged, and in
    // between is a linear crossfade - so a user can dial the EQ in without it ever being a bypass
    // that clicks. State (z1/z2) carries across calls: calling this twice in a row is not the same
    // as calling it once with the concatenation only in that the filter was not restarted, which is
    // the behaviour the tests pin.
    inline void processAudio(const AlignedVector& original_audio, double amount, AlignedVector& processed_output) noexcept {
        if (original_audio.empty()) { processed_output.clear(); return; }
        if (processed_output.size() != original_audio.size()) processed_output.resize(original_audio.size());
        float amt = static_cast<float>(amount);
        const float* src = original_audio.data();
        float* dst = processed_output.data();
        size_t n = original_audio.size();
        for (size_t i = 0; i < n; ++i) {
            float x = src[i];
            float filtered = m_low_shelf.process(m_high_shelf.process(x));
            dst[i] = x + amt * (filtered - x);
        }
    }

    // Streaming (stateful) processing of a block of NEW samples in place: data[i] = x + amount*(eq(x) - x).
    // Used at ingest so each sample is filtered exactly once, in time order, with continuous IIR state
    // (re-filtering the whole analysis window every frame is 4x the work and restarts the filter
    // from a stale state at every window start).
    inline void processBlockInPlace(float* data, size_t n, double amount) noexcept {
        const float amt = static_cast<float>(amount);
        for (size_t i = 0; i < n; ++i) {
            float x = data[i];
            float filtered = m_low_shelf.process(m_high_shelf.process(x));
            data[i] = x + amt * (filtered - x);
        }
    }

    // Clears both shelves' state, so the next audio starts from silence rather than from whatever
    // the filter was holding. Called when the input is restarted (the node's Reset pulse), so a
    // restart cannot ring the old signal into the new one.
    void reset() noexcept { m_high_shelf.reset(); m_low_shelf.reset(); }

private:
    double m_sample_rate{ 44100.0 };   // the rate the coefficients are designed for
    BiquadSection m_high_shelf;        // boost/cut above cutoffHz
    BiquadSection m_low_shelf;         // boost/cut below lowCutoffHz
    // The five parameter values m_high_shelf / m_low_shelf were designed from. An all-zero
    // initial value is deliberate: it cannot equal a real design (a real one would have to be
    // 0 dB gain at 0 Hz, which deactivates the shelf), so the first cook always designs.
    std::tuple<double, double, double, double, double> m_design_key;
};

/*
===========================================================================
 3. WINDOW GENERATOR
===========================================================================
WHAT: Builds the taper that is multiplied into each analysis window before the FFT.

WHY A WINDOW IS NEEDED AT ALL: an FFT only ever sees a finite slice of a continuous signal, which
      is the same as multiplying by a rectangle. A rectangle's spectrum has high sidelobes, so a
      strong tone leaks across the whole display as a raised floor. A taper trades a slightly wider
      main lobe (worse frequency resolution) for much lower sidelobes (less leakage), which is the
      trade the Window Type menu offers:
        Kaiser          - beta-tunable; the default (beta 15) is the low-sidelobe end
        Hann            - the general-purpose choice, -31 dB sidelobes
        Hamming         - similar to Hann but with a non-zero edge, -43 dB first sidelobe
        Blackman        - -58 dB sidelobes, wider main lobe
        BlackmanHarris  - -92 dB sidelobes, the quietest offered, widest main lobe
        Rectangular     - no taper at all; best resolution, worst leakage. Correct only for
                          signals that already fit the window exactly, or for measuring noise.

Normalization modes:
  CoherentGain : mean(window) == 1  (legacy behaviour; peak of a full-scale sine == N_win/2)
  FullScale    : sum(window) == 2   (one-sided spectrum of a sine with amplitude A reads A)

WHY NORMALIZATION IS NOT COSMETIC: it fixes what a bin's value means, which is what the dB
      reference in section 6c is measured against. CoherentGain keeps a full-scale sine reading
      N_win/2 in the magnitude spectrum, which is the historical behaviour every earlier version of
      this node produced and what an existing project's dB offsets were tuned against; FullScale
      makes the same sine read its own amplitude, which is what a dB-full-scale meter wants. Changing
      either one shifts every dB reading, so this is a compatibility knob, not a tuning knob.

HOW TO CHANGE - adding a window type needs three edits, in step:
      1. the WindowType enum in Parameters.h (its position is the menu order),
      2. the names/labels arrays in Parameters.cpp at the same position (the static_asserts there
         are what catch a mismatch),
      3. a `case` in generateWindow() below, whose integer matches that position.
      Miss (3) and the new menu entry silently builds the Kaiser window.
*/
enum class WindowNorm : int { CoherentGain = 0, FullScale = 1 };

class WindowGenerator {
public:
    // Modified Bessel function of the first kind, order 0, I0(x). Needed only by the Kaiser window
    // (it is the shape of that taper), where the argument is beta*sqrt(1-...), so it is evaluated
    // inside the per-sample loop and has to be cheap.
    //
    // Two polynomial approximations from Abramowitz & Stegun (9.8.1 for x < 3.75, 9.8.2 above),
    // accurate to about 1e-7 - far below what a float window coefficient can represent, so this is
    // not a source of error. The split at 3.75 is where the two series' accuracy bands meet, not a
    // tunable threshold; I0 grows like exp(x)/sqrt(x), which is why the upper branch factors that
    // out before evaluating the rational part.
    static double besselI0(double x) {
        double ax = std::abs(x);
        if (ax < 3.75) {
            double y = x / 3.75; y = y * y;
            return 1.0 + y * (3.5156229 + y * (3.0899424 + y * (1.2067492 + y * (0.2659732 + y * (0.0360768 + y * 0.0045813)))));
        }
        double y = 3.75 / ax;
        return (std::exp(ax) / std::sqrt(ax)) *
               (0.39894228 + y * (0.01328592 + y * (0.00225319 + y * (-0.00157565 + y * (0.00916281 + y * (-0.02057706 + y * (0.02635537 + y * (-0.01647633 + y * 0.00392377))))))));
    }

    // Fills `window` with `length` coefficients of the requested type and normalization.
    // window_type: 0 Kaiser, 1 Hann, 2 Hamming, 3 Blackman, 4 Blackman-Harris, 5 Rectangular
    //              (positions must match the WindowType enum in Parameters.h - see the section comment)
    // kaiser_beta: only read for type 0; larger = lower sidelobes, wider main lobe. 15 is the
    //              default and the value that makes Kaiser the low-leakage choice.
    // Produced in two passes rather than one: the coefficients are built unnormalized, their sum is
    // accumulated, and then every coefficient is scaled so that the sum (or the mean) hits the
    // target EXACTLY. Normalizing by a closed-form factor per window type would leave the result a
    // fraction of a percent off, and since these coefficients set the absolute level of every dB
    // reading downstream, "close enough" is not - the measured sum is the honest one.
    static void generateWindow(int window_type, double kaiser_beta, size_t length, AlignedVector& window,
                               WindowNorm norm = WindowNorm::CoherentGain) {
        window.resize(length);
        if (length == 0) return;
        // length-1, not length: this is the SYMMETRIC window, whose endpoints are exactly equal
        // (cos(0) and cos(2*pi)). The periodic form (denominator length) is the one for overlap-add
        // resynthesis, which this node does not do - it analyses one frame and discards it. Using
        // the wrong one is a small but real spectral error, so do not "fix" this to length.
        double denom = (length > 1) ? static_cast<double>(length - 1) : 1.0;
        double sum = 0.0;
        // Hoisted out of the loop: the Kaiser case divides by I0(beta) for every sample, and beta is
        // constant for the whole window.
        double kaiser_inv_I0 = 1.0 / besselI0(kaiser_beta);

        for (size_t n = 0; n < length; ++n) {
            double w = 1.0;
            double fn = static_cast<double>(n);
            switch (window_type) {
                case 1: w = 0.5 - 0.5 * std::cos(2.0 * PI_D * fn / denom); break;
                case 2: w = 0.54 - 0.46 * std::cos(2.0 * PI_D * fn / denom); break;
                case 3: w = 0.42 - 0.5 * std::cos(2.0 * PI_D * fn / denom) + 0.08 * std::cos(4.0 * PI_D * fn / denom); break;
                // The 4-term Blackman-Harris (its coefficients are the minimum-sidelobe set, which
                // is why they carry six decimal places and are not memorizable).
                case 4: w = 0.35875 - 0.48829 * std::cos(2.0 * PI_D * fn / denom)
                            + 0.14128 * std::cos(4.0 * PI_D * fn / denom)
                            - 0.01168 * std::cos(6.0 * PI_D * fn / denom); break;
                case 5: w = 1.0; break;
                case 0:
                default: {
                    // Kaiser: I0(beta*sqrt(1-t^2)) / I0(beta), with t sweeping -1..1 across the
                    // window. That placement of sqrt(1-t^2) (as an exponent-like envelope, applied to
                    // the Bessel argument rather than the result) is the definition - the I0 of the
                    // Bessel "ratio" form is not the same shape.
                    double term = 2.0 * fn / denom - 1.0;
                    double arg = std::sqrt(std::max(0.0, 1.0 - term * term));
                    w = besselI0(kaiser_beta * arg) * kaiser_inv_I0;
                    break;
                }
            }
            window[n] = static_cast<float>(w);
            sum += w;
        }
        // sum > 0 rather than != 0: only a pathological type could make it negative, and dividing by
        // a negative or zero sum would produce a window that is worse than leaving it unnormalized.
        if (sum > 0.0) {
            double target = (norm == WindowNorm::FullScale) ? 2.0 : static_cast<double>(length);
            float scale = static_cast<float>(target / sum);
            for (size_t n = 0; n < length; ++n) window[n] *= scale;
        }
    }
};

/*
===========================================================================
 4. PSYCHOACOUSTIC FREQUENCY SCALES & AVX2 GATHER INTERPOLATION
===========================================================================
WHAT: Turns the FFT's uniform grid of `n_linear_bins` magnitudes into the node's output grid of
      `n_out` bins, using one of six frequency scales (or a blend between linear and any of them).
      This is the "warp" stage, and it is a resampling of the magnitude curve - each output bin
      reads a linearly-interpolated (or cubic) value out of the input.

WHY A TABLE AND NOT A FORMULA PER SAMPLE: the mapping is fixed for a given set of parameters, so it
      is built once when a parameter changes (buildWarpTables) and then applied as a gather. That is
      what makes the per-cook cost a single indexed read per output bin rather than a log() or pow().

Tables: uint32 i0 (i1 = i0 + 1 implicit) + float weight = 8 bytes per output bin
(was 20 bytes with size_t i0/i1). AVX2 path gathers src[i0] and src[i0+1]
with a single index vector and blends with one FMA.

THE ONE INVARIANT THE TABLES MUST KEEP: every i0 is <= n_linear_bins - 2, because both the linear
      and the cubic kernels read src[i0 + 1] unconditionally. buildWarpTables enforces it by
      clamping to max_i0; if you change the index computation, that clamp has to survive, or the
      gather reads past the end of the magnitude buffer (a silent wrong answer, not a crash).
*/
class PerceptualWarping {
public:
    // R2C output bin count of an N-point real FFT: DC..Nyquist inclusive.
    static constexpr size_t linearBinCount(size_t fft_size) noexcept { return fft_size / 2 + 1; }

    /*
    The six scales, as monotonic forward/inverse pairs. Every one of them is only ever used as
    "sample the scale uniformly from scale(0) to scale(fmax), then invert back to Hz" (see
    computeTargetHzGrid). That matters more than the constants do, and it is worth being explicit
    about because it is easy to get wrong in the other direction:

      * the FIRST and LAST output frequencies are pinned exactly, whatever the constants are -
        the bottom is inverse(forward(0)) and the top is inverse(forward(fmax)), and the inverses
        are exact algebraic inverses of the forwards;
      * the constants therefore control only the SHAPE of the grid in between, and cannot make the
        axis start at the wrong frequency or stop short of fmax;
      * the units cancel by construction (note erbRateToHz multiplies by the same 123 that
        erbRateGlasberg divides by), so do not read these as standard-calibrated ERB numbers - they
        are an internal parameterization whose only job is to be monotonic and smoothly invertible.

    ERB is the quadratic Glasberg & Moore approximation. Bark and Mel are the usual HTK / Traunmuller
    style mappings. Chroma is a log2 pitch-class axis in semitones relative to A4. Two of these
    inverses have been wrong before and are pinned by tests now - see the note in barkToHz.
    */
    static double htkHzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
    static double htkMelToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

    static double erbRateGlasberg(double hz) { double x = hz / 123.0; return 6.230 * (x * x) + 93.390 * x + 28.520; }
    // Solved with the quadratic formula: x = (-b + sqrt(b^2 - 4a(c - erb))) / 2a, taking the
    // positive root because the forward function is increasing on the domain that matters (x >= 0).
    static double erbRateToHz(double erb) {
        double a = 6.230, b = 93.390, c = 28.520;
        double x = (-b + std::sqrt(std::max(0.0, b * b - 4.0 * a * (c - erb)))) / (2.0 * a);
        return x * 123.0;
    }

    // Traunmuller's Bark approximation with the piecewise corrections that make it match the
    // critical-band table at the low and high ends.
    static double hzToBark(double hz) {
        double z = (26.81 * hz) / (1960.0 + hz) - 0.53;
        if (z < 2.0) z += 0.15 * (2.0 - z);
        else if (z > 20.1) z += 0.22 * (z - 20.1);
        return z;
    }
    static double barkToHz(double bark) {
        double z = bark;
        // Exact inverse of the piecewise extensions above. The upper branch of hzToBark adds
        // 0.22*(z-20.1), i.e. z' = 1.22*z - 4.422, so undoing it is (z+4.422)/1.22 (it used to
        // divide by 0.78, which is the inverse of a different line and only agrees at z = 20.1:
        // past ~6.5 kHz the axis folded over — non-monotonic, top bin back down at 0 Hz — for any
        // fmax above that, including the 44.1 kHz Nyquist).
        if (z < 2.0) z = (z - 0.3) / 0.85;
        else if (z > 20.1) z = (z + 4.422) / 1.22;
        double f = (1960.0 * (z + 0.53)) / (26.81 - (z + 0.53));
        return std::max(0.0, f);
    }

    static double hzToChroma(double hz) { double f = std::max(1e-5, hz); return 12.0 * std::log2(f / 440.0) + 69.0; }
    static double chromaToHz(double chroma) { return 440.0 * std::pow(2.0, (chroma - 69.0) / 12.0); }

    // Fills target_hz with one frequency per output bin: the grid the output axis actually has.
    //
    // scale_code: 0 Log, 1 Mel, 2 ERB, 3 Bark, 4 Chroma, 5 Linear, 6 Mel+Log blend
    // warp_blend: 0 = a plain linear-in-Hz axis whatever `scale_code` says, 1 = the scale's own
    //             axis, in between = a linear crossfade of the two. This is the Warp Blend slider,
    //             and it is why scale_code 5 (Linear) and blend 0 produce the same axis.
    //
    // HOW IT IS BUILT, in two steps that are worth keeping separate in your head:
    //   1. sample the chosen scale uniformly: perceptual[i] = inverse(min + (i/(n_out-1))*(max-min)),
    //      where min/max are the scale evaluated at 0 and at fmax. Because the inverse is the exact
    //      algebraic inverse, perceptual[0] and perceptual[n_out-1] land exactly on the scale's own
    //      endpoints - and for the linear-in-Hz scales that is exactly 0 and fmax.
    //   2. crossfade with the linear grid and clamp into [0, fmax].
    // The clamp in step 2 is what makes the last bin EXACTLY fmax for every scale and every blend,
    // which is the property the axis-rate derivation depends on (see targetHz() below and
    // AnalysisPipeline::updateWarp). A blend of two curves that both end at fmax cannot exceed it,
    // but the clamp makes that a guarantee rather than an argument.
    static void computeTargetHzGrid(int scale_code, double fmax, size_t n_out, double warp_blend, double log_floor_hz,
                                    std::vector<double>& target_hz) {
        target_hz.resize(n_out);
        if (n_out == 0) return;
        double inv_denom = (n_out > 1) ? 1.0 / static_cast<double>(n_out - 1) : 0.0;
        std::vector<double> perceptual(n_out);
        switch (scale_code) {
            case 0: {
                double log_min = std::log(std::max(1.0, log_floor_hz)), log_max = std::log(fmax);
                for (size_t i = 0; i < n_out; ++i) perceptual[i] = std::exp(log_min + i * inv_denom * (log_max - log_min));
                break;
            }
            case 1: {
                double m_min = htkHzToMel(0.0), m_max = htkHzToMel(fmax);
                for (size_t i = 0; i < n_out; ++i) perceptual[i] = htkMelToHz(m_min + i * inv_denom * (m_max - m_min));
                break;
            }
            case 2: {
                double e_min = erbRateGlasberg(0.0), e_max = erbRateGlasberg(fmax);
                for (size_t i = 0; i < n_out; ++i) perceptual[i] = erbRateToHz(e_min + i * inv_denom * (e_max - e_min));
                break;
            }
            case 3: {
                double b_min = hzToBark(0.0), b_max = hzToBark(fmax);
                for (size_t i = 0; i < n_out; ++i) perceptual[i] = barkToHz(b_min + i * inv_denom * (b_max - b_min));
                break;
            }
            case 4: {
                // Chroma is pitch classes, i.e. a log2 axis, so it needs a positive bottom: the fixed
                // 20 Hz floor is the lowest frequency whose pitch class is still meaningful. Log Floor Hz
                // is deliberately NOT consulted here - it is a Log/Mel+Log parameter, and chroma has no
                // use for it (a log2 axis cannot start at 0 Hz, which is the value Log Floor guards).
                double c_min = hzToChroma(20.0), c_max = hzToChroma(fmax);
                for (size_t i = 0; i < n_out; ++i) perceptual[i] = chromaToHz(c_min + i * inv_denom * (c_max - c_min));
                break;
            }
            case 6: {
                double m_min = htkHzToMel(0.0), m_max = htkHzToMel(fmax);
                double log_min = std::log(std::max(1.0, log_floor_hz)), log_max = std::log(fmax);
                for (size_t i = 0; i < n_out; ++i) {
                    double frac = i * inv_denom;
                    perceptual[i] = 0.5 * (htkMelToHz(m_min + frac * (m_max - m_min)) + std::exp(log_min + frac * (log_max - log_min)));
                }
                break;
            }
            case 5:
            default:
                for (size_t i = 0; i < n_out; ++i) perceptual[i] = i * inv_denom * fmax;
                break;
        }
        for (size_t i = 0; i < n_out; ++i) {
            double lin = i * inv_denom * fmax;
            double axis_val = (1.0 - warp_blend) * lin + warp_blend * perceptual[i];
            target_hz[i] = std::max(0.0, std::min(fmax, axis_val));
        }
    }

    // One point of the grid computeTargetHzGrid() builds, at a fractional position frac in [0, 1]
    // along the axis (frac = i / (n_out - 1)). Same formulas, same evaluation order, so
    // targetHzAt(.., i/(n-1), ..) equals computeTargetHzGrid(..)[i] (pinned by a test). Used where a
    // property of the grid is needed without building it - Output Bins Mode = Auto, per cook.
    static double targetHzAt(int scale_code, double fmax, double frac, double warp_blend, double log_floor_hz) {
        double per;
        switch (scale_code) {
            case 0: { double lmin = std::log(std::max(1.0, log_floor_hz)), lmax = std::log(fmax);
                      per = std::exp(lmin + frac * (lmax - lmin)); break; }
            case 1: { double m0 = htkHzToMel(0.0), m1 = htkHzToMel(fmax); per = htkMelToHz(m0 + frac * (m1 - m0)); break; }
            case 2: { double e0 = erbRateGlasberg(0.0), e1 = erbRateGlasberg(fmax); per = erbRateToHz(e0 + frac * (e1 - e0)); break; }
            case 3: { double b0 = hzToBark(0.0), b1 = hzToBark(fmax); per = barkToHz(b0 + frac * (b1 - b0)); break; }
            case 4: { double c0 = hzToChroma(20.0), c1 = hzToChroma(fmax); per = chromaToHz(c0 + frac * (c1 - c0)); break; }
            case 6: { double m0 = htkHzToMel(0.0), m1 = htkHzToMel(fmax);
                      double lmin = std::log(std::max(1.0, log_floor_hz)), lmax = std::log(fmax);
                      per = 0.5 * (htkMelToHz(m0 + frac * (m1 - m0)) + std::exp(lmin + frac * (lmax - lmin))); break; }
            case 5:
            default: per = frac * fmax; break;
        }
        const double lin = frac * fmax;
        return std::max(0.0, std::min(fmax, (1.0 - warp_blend) * lin + warp_blend * per));
    }

    // d(target Hz)/d(frac) at the top of the axis: how many Hz one unit of axis position spans where the
    // grid is coarsest. Every scale here is convex in frac (spacing grows with frequency), so the widest
    // gap between two output bins is at the top, and it is ~ topSlopeHzPerFrac / (n_out - 1).
    static double topSlopeHzPerFrac(int scale_code, double fmax, double warp_blend, double log_floor_hz) {
        const double e = 1e-4;
        return (targetHzAt(scale_code, fmax, 1.0, warp_blend, log_floor_hz) -
                targetHzAt(scale_code, fmax, 1.0 - e, warp_blend, log_floor_hz)) / e;
    }

    // 0 = linear (2 taps), 1 = Catmull-Rom cubic (4 taps, smoother lobes -> allows a smaller FFT)
    // Cubic costs two more gathers per output bin; it is worth it when the output grid is much
    // finer than the FFT's linear grid, where a linear read shows the magnitude curve's corners.
    // Any value other than 1 selects linear, so an out-of-range parameter cannot corrupt a kernel.
    void setInterpolation(int mode) noexcept { m_interp = (mode == 1) ? 1 : 0; }
    int interpolation() const noexcept { return m_interp; }

    // What a coarse output bin reports (one that covers two or more FFT bins): 0 = the interpolated value
    // (legacy), 1 = the peak of its FFT-bin range, 2 = the RMS (power mean) of the range. Takes effect at
    // the next buildWarpTables(). Where the output grid is finer than the FFT grid nothing changes.
    // WHY: interpolation reads two FFT bins per output bin, so with fewer output bins than FFT bins the
    // bins in between are never read - a narrow partial that falls between two taps loses level and
    // flickers as it moves. Peak aggregation is what makes a small Output Bins count safe for display.
    void setAggregation(int mode) noexcept { m_agg = (mode == 1 || mode == 2) ? mode : 0; }
    int aggregation() const noexcept { return m_agg; }
    // How many output bins are aggregated (cover >= 2 FFT bins) with the current tables. Telemetry.
    size_t aggregatedBins() const noexcept { return m_agg_idx.size(); }

    // Highest linear bin index the warp reads (+ cubic look-ahead). Magnitudes above it need not be computed.
    size_t maxLinearIndex() const noexcept { return m_max_index; }

    // Builds the mapping tables for one set of parameters. Called whenever the scale, axis or bin
    // count changes - not per cook (see AnalysisPipeline::updateWarp, which caches on a key).
    //
    // The mapping per output bin i is: "where does target_hz[i] sit on the FFT's own grid?"
    //     frac = target_hz[i] / nyquist * (nlin-1)     <- position in linear-bin units
    //     i0   = floor(frac), clamped to [0, nlin-2]
    //     w    = frac - i0, clamped to [0, 1]          <- interpolation weight toward i0+1
    // so applying the warp is src[i0] + w*(src[i0+1] - src[i0]) - one gather pair and one FMA.
    void buildWarpTables(int scale_code, double fmax, size_t n_out, double nyquist, double warp_blend, double log_floor_hz, size_t nlin) {
        computeTargetHzGrid(scale_code, fmax, n_out, warp_blend, log_floor_hz, m_target_hz);
        m_i0.resize(n_out);
        m_w.resize(n_out);
        m_nlin = nlin;
        m_max_index = 0;
        double denom = (nlin > 1) ? static_cast<double>(nlin - 1) : 1.0;
        bool is_id = (n_out == nlin);
        // The clamp described in this class's header comment, and the reason it is not nlin-1:
        // every kernel reads src[i0+1], so the largest usable i0 leaves one bin of headroom.
        const size_t max_i0 = (nlin >= 2) ? nlin - 2 : 0;
        for (size_t i = 0; i < n_out; ++i) {
            double frac = (nyquist > 0.0) ? (m_target_hz[i] / nyquist) * denom : 0.0;
            // Snap positions that are an integer up to rounding noise, so an exact 1:1 grid
            // (Linear scale with bins == linear bins) is detected as identity (memcpy bypass).
            // Without it, a value computed as 2047.9999999998 would floor to 2047 with w = 1.0,
            // which is the same answer but would fail the is_id test below and cost a full gather
            // pass for a grid that is bit-identical to the input.
            double rounded = std::round(frac);
            if (std::abs(frac - rounded) < 1e-6) frac = rounded;
            size_t i0_val = static_cast<size_t>(std::max(0.0, std::min(static_cast<double>(max_i0), std::floor(frac))));
            float weight = static_cast<float>(std::clamp(frac - static_cast<double>(i0_val), 0.0, 1.0));
            m_i0[i] = static_cast<uint32_t>(i0_val);
            m_w[i] = weight;
            // How far up the linear grid this output bin reaches. +2 rather than +1 because the
            // cubic kernel reads one bin further ahead (src[i0+2]); for the linear kernel it asks
            // for one bin more than it needs, which only costs a little extra magnitude work in the
            // FFT stage and is why maxLinearIndex() is described as a bound, not an exact count.
            m_max_index = std::max(m_max_index, std::min(nlin - 1, i0_val + 2));
            // identity iff every output bin samples exactly linear bin i (the last bin is
            // represented as i0 = nlin-2 with weight 1.0 because of the clamp above).
            // i0_val + weight is the effective sampled position, so this test is exact for both the
            // clamped last bin and the unclamped middle ones.
            if (is_id && std::abs((static_cast<double>(i0_val) + weight) - static_cast<double>(i)) > 1e-5) is_id = false;
        }
        m_is_identity = is_id;

        // Aggregation ranges. Output bin i owns the FFT bins between the midpoints to its neighbours
        // (in linear-bin units): [ceil((p[i-1]+p[i])/2), floor((p[i]+p[i+1])/2)]. Only bins that own two
        // or more FFT bins are recorded - everywhere else the interpolated value already reads the
        // nearest FFT bins, and a table entry would only cost time.
        m_agg_idx.clear(); m_agg_lo.clear(); m_agg_cnt.clear();
        if (m_agg != 0 && !is_id && n_out >= 2 && nlin >= 2 && nyquist > 0.0) {
            auto pos = [&](size_t i) { return (m_target_hz[i] / nyquist) * denom; };
            const double last = static_cast<double>(nlin - 1);
            for (size_t i = 0; i < n_out; ++i) {
                const double lo_pos = (i == 0) ? pos(0) : 0.5 * (pos(i - 1) + pos(i));
                const double hi_pos = (i + 1 == n_out) ? pos(i) : 0.5 * (pos(i) + pos(i + 1));
                const double lo_d = std::max(0.0, std::ceil(lo_pos - 1e-9));
                const double hi_d = std::min(last, std::floor(hi_pos + 1e-9));
                if (hi_d - lo_d < 1.0) continue;                       // owns 0 or 1 FFT bin: interpolation is right
                const size_t lo = static_cast<size_t>(lo_d), hi = static_cast<size_t>(hi_d);
                m_agg_idx.push_back(static_cast<uint32_t>(i));
                m_agg_lo.push_back(static_cast<uint32_t>(lo));
                m_agg_cnt.push_back(static_cast<uint32_t>(hi - lo + 1));
                m_max_index = std::max(m_max_index, hi);
            }
        }
        // The trailing run of aggregated bins (on a Log/Mel/... axis: every bin from where the output
        // spacing passes 2 FFT bins to the top) is fully overwritten by applyAggregation, so the
        // interpolation pass stops where that run starts - it was computing values nobody reads, and on
        // the gather path (the coarse part of the axis is exactly where the permute path does not fit).
        m_interp_end = n_out;
        for (size_t k = m_agg_idx.size(); k > 0 && m_agg_idx[k - 1] + 1 == m_interp_end; --k) --m_interp_end;
    }

    // Fills output_spectrum (resized to the table length) with the warped magnitudes.
    // The identity case is a memcpy, which is not a micro-optimization: it is the exact path a
    // whole-FFT linear grid takes, and it is what guarantees that configuration's output is
    // bit-identical to the FFT's own bins rather than a resampled approximation of them.
    inline void applyWarp(const AlignedVector& linear_magnitude, AlignedVector& output_spectrum) const noexcept {
        size_t n_out = m_i0.size();
        if (output_spectrum.size() != n_out) output_spectrum.resize(n_out);
        if (n_out == 0) return;
        if (m_is_identity && linear_magnitude.size() == n_out) {
            std::memcpy(output_spectrum.data(), linear_magnitude.data(), n_out * sizeof(float));
            return;
        }
        const float* src = linear_magnitude.data();
        float* dst = output_spectrum.data();
        const uint32_t* idx = m_i0.data();
        const float* w_ptr = m_w.data();
        size_t i = 0;
        // With aggregation on, the bins from m_interp_end up are all overwritten by applyAggregation.
        const size_t n_interp = (m_agg != 0 && m_interp_end <= n_out) ? m_interp_end : n_out;
        if (m_interp == 1) {
            // Cubic handles its own tail and its own fallback; it never falls through to the loop below.
            applyWarpCubic(src, dst, idx, w_ptr, n_interp, linear_magnitude.size());
            applyAggregation(src, linear_magnitude.size(), dst);
            return;
        }
#if defined(__AVX2__)
        // Guard: gathers index src[i0+1]; tables guarantee i0 <= nlin-2 <= src.size()-2.
        // The size test is a belt-and-braces check on that table invariant, not a data-dependent
        // branch: when it fails (only possible if the caller hands in a shorter magnitude array)
        // the vector block is skipped and the scalar tail below - which reads src[i0] and src[i0+1]
        // unconditionally too - is what actually runs. So the guard does not make that case safe;
        // callers must still pass at least m_nlin magnitudes. What it does guarantee is that the
        // SIMD path never gathers out of bounds, keeping a caller bug a scalar read rather than a
        // fault inside a vector instruction where the cause is far harder to see.
        //
        // LOAD + PERMUTE instead of GATHER where it fits. i0 is non-decreasing along the axis, so when
        // the 8 output bins of a vector read taps inside one 8-float window (i0[i+7] - i0[i] <= 7: the
        // fine, upsampled part of the axis - ~83 % of the vectors at the 16384-bin Log default), the
        // taps are one unaligned load + one in-register vpermps each instead of an 8-element gather.
        // Same values, same FMA: the output is bit-identical to the gather path (tests pin it).
        // Measured 4.0 -> 2.2 us at 16384 bins (i9-13900H P-core). The span test is a well-predicted
        // branch: it flips once, where the axis becomes coarser than the FFT grid.
        const size_t src_n = linear_magnitude.size();
        if (src_n >= m_nlin) {
            for (; i + 7 < n_interp; i += 8) {
                const __m256i vi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(idx + i));
                const uint32_t base = idx[i];
                __m256 v0, v1;
                if (idx[i + 7] - base <= 7 && base + 9 <= src_n) {
                    const __m256i rel = _mm256_sub_epi32(vi, _mm256_set1_epi32(static_cast<int>(base)));
                    v0 = _mm256_permutevar8x32_ps(_mm256_loadu_ps(src + base), rel);
                    v1 = _mm256_permutevar8x32_ps(_mm256_loadu_ps(src + base + 1), rel);
                } else {
                    v0 = _mm256_i32gather_ps(src, vi, 4);
                    v1 = _mm256_i32gather_ps(src + 1, vi, 4);
                }
                __m256 w = _mm256_loadu_ps(w_ptr + i);
                _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(w, _mm256_sub_ps(v1, v0), v0));
            }
        }
#endif
        // Scalar tail: handles a non-AVX2 build entirely, and the last <8 bins otherwise.
        // Same formula as the vector block and deliberately written in the same order
        // (v0 + w * (v1 - v0)) so a build with and without AVX2 agrees to the last bit.
        for (; i < n_interp; ++i) {
            size_t i0 = idx[i];
            float w = w_ptr[i];
            dst[i] = src[i0] + w * (src[i0 + 1] - src[i0]);
        }
        applyAggregation(src, linear_magnitude.size(), dst);
    }

    // Overwrites the coarse output bins with the peak (or RMS) of the FFT bins each one owns. Runs after
    // the interpolation pass, so the fine part of the axis keeps its interpolated values. Total work is
    // at most one read of every FFT bin (the ranges do not overlap except at shared midpoints).
    //
    // WHY IT IS WRITTEN BRANCH-FREE: a range is 2..~50 FFT bins and its length changes from one output
    // bin to the next, so a plain `for (j < cnt)` loop mispredicts its exit on almost every bin and runs a
    // serial max chain - measured 4-9 us for ~1000 coarse bins. Here each range is read in 8-wide chunks
    // (a short, well-predicted loop: the chunk count changes rarely along the axis) and the lanes past the
    // range are masked out, then one horizontal reduction per bin: ~1-2 us for the same work.
    void applyAggregation(const float* __restrict src, size_t src_size, float* __restrict dst) const noexcept {
        const size_t n = m_agg_idx.size();
        if (m_agg == 0 || n == 0) return;
        // Local copies: MSVC does not use type-based aliasing, so with members it reloads every vector's
        // data pointer after each store to dst.
        const uint32_t* __restrict aidx = m_agg_idx.data();
        const uint32_t* __restrict alo = m_agg_lo.data();
        const uint32_t* __restrict acnt = m_agg_cnt.data();
        const int mode = m_agg;
        size_t k = 0;
#if defined(__AVX2__)
        // 16 entries: loading 8 of them starting at (8 - rem) gives `rem` all-ones lanes followed by zeros.
        alignas(32) static const int32_t kMask[16] = { -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0 };
        const __m256 vneg = _mm256_set1_ps(-3.402823466e+38f);
        for (; k < n; ++k) {
            const size_t lo = alo[k];
            const size_t cnt = acnt[k];
            const size_t chunks = (cnt + 7) / 8;
            if (lo + chunks * 8 > src_size) break;                     // the last ranges near the end: scalar tail
            const float* p = src + lo;
            if (mode == 1) {
                __m256 vm = vneg;
                for (size_t c = 0; c + 1 < chunks; ++c) vm = _mm256_max_ps(vm, _mm256_loadu_ps(p + c * 8));
                const size_t rem = cnt - (chunks - 1) * 8;             // 1..8 valid lanes in the last chunk
                const __m256 mask = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(kMask + 8 - rem)));
                vm = _mm256_max_ps(vm, _mm256_blendv_ps(vneg, _mm256_loadu_ps(p + (chunks - 1) * 8), mask));
                __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(vm), _mm256_extractf128_ps(vm, 1));
                m4 = _mm_max_ps(m4, _mm_movehl_ps(m4, m4));
                m4 = _mm_max_ss(m4, _mm_shuffle_ps(m4, m4, 1));
                dst[aidx[k]] = _mm_cvtss_f32(m4);
            } else {
                __m256 acc = _mm256_setzero_ps();
                for (size_t c = 0; c + 1 < chunks; ++c) { const __m256 v = _mm256_loadu_ps(p + c * 8); acc = _mm256_fmadd_ps(v, v, acc); }
                const size_t rem = cnt - (chunks - 1) * 8;
                const __m256 mask = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(kMask + 8 - rem)));
                const __m256 v = _mm256_and_ps(_mm256_loadu_ps(p + (chunks - 1) * 8), mask);
                acc = _mm256_fmadd_ps(v, v, acc);
                __m128 s4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
                s4 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
                s4 = _mm_add_ss(s4, _mm_shuffle_ps(s4, s4, 1));
                dst[aidx[k]] = std::sqrt(_mm_cvtss_f32(s4) / static_cast<float>(cnt));
            }
        }
#endif
        // Scalar path: a non-AVX2 build, and the few ranges whose 8-wide reads would pass the buffer end.
        for (; k < n; ++k) {
            const size_t lo = alo[k];
            size_t cnt = acnt[k];
            if (lo >= src_size) { dst[aidx[k]] = 0.0f; continue; }     // caller handed fewer bins than the tables expect (the bin may not have been interpolated)
            if (lo + cnt > src_size) cnt = src_size - lo;
            const float* p = src + lo;
            if (mode == 1) {
                float m = p[0];
                for (size_t j = 1; j < cnt; ++j) m = std::max(m, p[j]);
                dst[aidx[k]] = m;
            } else {
                double acc = 0.0;
                for (size_t j = 0; j < cnt; ++j) acc += static_cast<double>(p[j]) * p[j];
                dst[aidx[k]] = static_cast<float>(std::sqrt(acc / static_cast<double>(cnt)));
            }
        }
    }

    // Catmull-Rom cubic interpolation - the smoother kernel, selected by setInterpolation(1).
    //
    // WHAT: reads four neighbouring linear bins around each output bin and evaluates the
    // Catmull-Rom polynomial at the fractional position w. It is a pass-through at integer
    // positions (w = 0 returns p1 exactly), so it degrades to the linear kernel's answer when the
    // grid happens to line up, and only changes the shape in between.
    //
    // WHY IT EXISTS: with only two taps the warped spectrum shows a straight-line segment between
    // every pair of linear bins, which reads as visible corners on a log axis where the low bins
    // are stretched far apart. Four taps round those corners off, which is what allows a smaller
    // FFT to be used for a given visual smoothness - the trade is two extra gathers per bin.
    //
    // WHAT TO CHANGE IF YOU EVER NEED TO: the four surrounding indices are clamped, not gathered
    // with wraparound - p0 clamps up to 0 and p2/p3 clamp down to the last bin - so the two end
    // bins interpolate against a duplicated edge sample rather than reading off the array. Any
    // rewrite has to keep that clamping, because the tables only guarantee i0 <= nlin-2 and the
    // cubic needs i0-1 and i0+2 as well. The vectorized body is untested by construction against
    // the scalar one except through the vectorization tests; keep both in the polynomial form
    // written out in the line comment below, and keep the final max(0, v) (a cubic overshoot at
    // a spectral peak would otherwise put a negative magnitude into the dB path).
    void applyWarpCubic(const float* __restrict src, float* __restrict dst, const uint32_t* __restrict idx,
                        const float* __restrict w_ptr, size_t n_out, size_t src_size) const noexcept {
        // Too few bins for four taps (or a magnitude array shorter than the tables expect) -
        // fall back to the linear kernel, which the guard above has already shown is in range.
        if (src_size < 4 || src_size < m_nlin) {
            for (size_t i = 0; i < n_out; ++i) { size_t i0 = idx[i]; dst[i] = src[i0] + w_ptr[i] * (src[i0 + 1] - src[i0]); }
            return;
        }
        const int last = static_cast<int>(src_size) - 1;
        size_t i = 0;
#if defined(__AVX2__)
        const __m256i v_zero = _mm256_setzero_si256();
        const __m256i v_last = _mm256_set1_epi32(last);
        const __m256i one = _mm256_set1_epi32(1), two = _mm256_set1_epi32(2);
        const __m256 h = _mm256_set1_ps(0.5f), c2 = _mm256_set1_ps(2.0f), c3 = _mm256_set1_ps(3.0f),
                     c4 = _mm256_set1_ps(4.0f), c5 = _mm256_set1_ps(5.0f), fz = _mm256_setzero_ps();
        // Load + permute where the 8 output bins' taps fit one window (see applyWarp): the four taps
        // i0-1 .. i0+2 are then four overlapping unaligned loads + vpermps, and none of the edge clamps
        // can bind (base >= 1 and base + 9 <= last). Bit-identical to the gathers; 7.6 -> 4.8 us at
        // 16384 bins (i9-13900H P-core).
        for (; i + 7 < n_out; i += 8) {
            const __m256i vi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(idx + i));
            const uint32_t base = idx[i];
            __m256 p0, p1, p2, p3;
            if (base >= 1 && idx[i + 7] - base <= 7 && base + 10 <= src_size) {
                const __m256i rel = _mm256_sub_epi32(vi, _mm256_set1_epi32(static_cast<int>(base)));
                p0 = _mm256_permutevar8x32_ps(_mm256_loadu_ps(src + base - 1), rel);
                p1 = _mm256_permutevar8x32_ps(_mm256_loadu_ps(src + base), rel);
                p2 = _mm256_permutevar8x32_ps(_mm256_loadu_ps(src + base + 1), rel);
                p3 = _mm256_permutevar8x32_ps(_mm256_loadu_ps(src + base + 2), rel);
            } else {
                p0 = _mm256_i32gather_ps(src, _mm256_max_epi32(_mm256_sub_epi32(vi, one), v_zero), 4);
                p1 = _mm256_i32gather_ps(src, vi, 4);
                p2 = _mm256_i32gather_ps(src, _mm256_min_epi32(_mm256_add_epi32(vi, one), v_last), 4);
                p3 = _mm256_i32gather_ps(src, _mm256_min_epi32(_mm256_add_epi32(vi, two), v_last), 4);
            }
            __m256 t = _mm256_loadu_ps(w_ptr + i);
            __m256 a = _mm256_sub_ps(p2, p0);                                                          // -p0 + p2
            __m256 b = _mm256_sub_ps(_mm256_fmadd_ps(c4, p2, _mm256_fmsub_ps(c2, p0, _mm256_mul_ps(c5, p1))), p3); // 2p0-5p1+4p2-p3
            __m256 c = _mm256_add_ps(_mm256_fmsub_ps(c3, p1, _mm256_add_ps(p0, _mm256_mul_ps(c3, p2))), p3);       // -p0+3p1-3p2+p3
            __m256 poly = _mm256_fmadd_ps(_mm256_fmadd_ps(_mm256_fmadd_ps(c, t, b), t, a), t, _mm256_mul_ps(c2, p1));
            _mm256_storeu_ps(dst + i, _mm256_max_ps(_mm256_mul_ps(h, poly), fz));
        }
#endif
        for (; i < n_out; ++i) {
            int i0 = static_cast<int>(idx[i]);
            float p0 = src[std::max(i0 - 1, 0)], p1 = src[i0], p2 = src[std::min(i0 + 1, last)], p3 = src[std::min(i0 + 2, last)];
            float t = w_ptr[i];
            float v = 0.5f * (2.0f * p1 + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
            dst[i] = std::max(0.0f, v);
        }
    }

    /*
    The frequency axis this grid describes, in Hz, one entry per output bin. m_target_hz[0] is
    DC for the linear-in-Hz scales (Mel, ERB, Bark, Linear) and the log floor (~20 Hz) for the
    logarithmic family, which cannot contain 0; m_target_hz[n_out-1] is exactly fmax for all of
    them, verified monotonic for every scale.

    This is what the CHOP's reported sample rate is derived from, and the derivation is exact
    rather than fitted. A CHOP sample rate normally counts time samples per second; for a
    spectrum the coherent reading is "the axis spans 0..sr_out/2", i.e. the last bin sits on
    Nyquist, so

        sr_out = 2 * m_target_hz[n_out-1] = 2 * fmax

    for every grid — uniform, warped, or linear. Two consequences worth checking by hand:
    the whole-FFT raw grid (fmax = sr_in/2) reports exactly sr_in, and per-bin spacing is
    sr_out / (2*(n_out-1)), which for that raw grid is sr_in/fft_size, the FFT's own resolution.
    */
    // The three query accessors below are what the rest of the plugin reads this object through.
    // targetHz() is the important one: it is the axis the CHOP reports the sample rate from (see the
    // block comment above), and AnalysisPipeline::status() reads targetHz().front() for the axis
    // bottom. isIdentity() is read to pick the memcpy bypass and to decide whether the axis is a
    // plain linear one. outputBins() is the contract for how long the output spectrum must be.
    const std::vector<double>& targetHz() const noexcept { return m_target_hz; }
    bool isIdentity() const noexcept { return m_is_identity; }
    size_t outputBins() const noexcept { return m_i0.size(); }

private:
    std::vector<double> m_target_hz;   // axis in Hz, one entry per output bin (kept as double: the
                                       // rate derivation is done in double and this is its input)
    std::vector<uint32_t> m_i0;        // source bin index of the lower tap, per output bin
    std::vector<float> m_w;            // weight toward the upper tap, per output bin
    size_t m_nlin{ 0 };                // source (linear) bin count the tables were built against -
                                       // applyWarp compares the incoming magnitude size to it
    size_t m_max_index{ 0 };           // see maxLinearIndex()
    int m_interp{ 0 };                 // 0 linear, 1 cubic
    int m_agg{ 0 };                    // 0 off, 1 peak, 2 rms (setAggregation)
    std::vector<uint32_t> m_agg_idx, m_agg_lo, m_agg_cnt;   // coarse output bins and the FFT-bin range each owns
    size_t m_interp_end{ 0 };          // interpolate [0, m_interp_end); the rest is all aggregated (buildWarpTables)
    bool m_is_identity{ false };       // see buildWarpTables()
};

/*
===========================================================================
 5. EQUAL-LOUDNESS WEIGHTING CURVES (IEC 61672 / ITU-R 468)
===========================================================================
Turns the frequency axis into a per-bin gain, so that a spectrum drawn through it reads the way
the ear hears it rather than the way a microphone measures it: a 50 Hz rumble and a 5 kHz tone at
the same physical level stop looking equally loud, which is what makes the display useful for
judging a mix by eye.

WHAT EACH CURVE IS FOR (weighting_code):
  0  off            no weighting - a flat multiply, and the pipeline skips the weighting stage
  1  A-weighting    the classic "how loud does this seem" curve from IEC 61672; strongly rolls off
                    the low end. Use it when the user is asking "is this annoying/loud?"
  2  C-weighting    nearly flat across the audio band, gently rolled off below ~30 Hz. Use it when
                    the user wants near-true level but without DC/rumble dominating.
  3  ITU-R 468      the "how much does this hiss annoy" curve; its high-frequency boost makes it
                    the right one for judging noise and codec artefacts rather than music.

HOW TO CHANGE: the code is an index into the same enum the UI dropdown uses (Parameters.h,
Weighting), and both the names and this switch have to gain a case together - the same three-place
rule as the window types. Anything unrecognised falls through with w = 1.0, i.e. it silently
becomes "off" rather than failing.

COST: one call per axis rebuild, not per cook - the curve depends only on the frequency axis. The
result is cached by AnalysisPipeline::updateWeighting against a key, so a change here costs one
pass over the bins and nothing afterwards.
*/
class EqualLoudness {
public:
    // Fills weights[] with one gain per entry of freqs_hz (so the two must be the same length -
    // the function resizes weights to match rather than assuming). Every curve is a pure function
    // of frequency: no state, no history, safe to call from any thread on its own buffers.
    // weighting_code: 0 off, 1 A, 2 C, 3 ITU-R 468
    static void computeCurve(int weighting_code, const std::vector<double>& freqs_hz, AlignedVector& weights) {
        weights.resize(freqs_hz.size());
        if (weighting_code == 0) { std::fill(weights.begin(), weights.end(), 1.0f); return; }
        // A and C are defined up to a constant, and the standard fixes that constant by the value at
        // 1 kHz. So: evaluate the same response at f = 1 kHz and divide it out, which puts the curve
        // at exactly 0 dB there. f1k is f^2 at 1 kHz (1e6), because the response formulas below are
        // written in f^2 (and f^4 for A) rather than f, so 1 kHz has to arrive pre-squared to match.
        //
        // This normalization is what makes the curves comparable across axis lengths and why the
        // numbers are read in the standard's own units. Do not "simplify" it away by baking a
        // constant: the value below is the response formula evaluated at 1 kHz, deliberately, so
        // that a typo in the formula shows up as a wrong curve in both places at once instead of
        // hiding behind a hand-copied magic number.
        double inv_ref = 1.0;
        if (weighting_code == 1) {
            const double f1k = 1e6;
            double ra_1k = ((12194.0 * 12194.0) * (f1k * f1k)) / ((f1k + 20.6 * 20.6) * std::sqrt((f1k + 107.7 * 107.7) * (f1k + 737.9 * 737.9)) * (f1k + 12194.0 * 12194.0));
            inv_ref = 1.0 / ra_1k;
        } else if (weighting_code == 2) {
            const double f1k = 1e6;
            double rc_1k = ((12194.0 * 12194.0) * f1k) / ((f1k + 20.6 * 20.6) * (f1k + 12194.0 * 12194.0));
            inv_ref = 1.0 / rc_1k;
        }
        for (size_t i = 0; i < freqs_hz.size(); ++i) {
            // DC cannot be weighted (every formula divides by a term containing f), and a log axis
            // starts near 20 Hz but a linear one starts at exactly 0, so the floor is load-bearing
            // for linear axes rather than a safety net. 1e-5 Hz gives the same answer as 0 would in
            // the limit - the curve is ~0 there either way - without a division by zero.
            double f = std::max(1e-5, freqs_hz[i]);
            double w = 1.0;   // default: unweighted, which is also the fallback for an unknown code
            if (weighting_code == 1) {
                // IEC 61672 A-weighting: two high-pass-ish poles (20.6, 12194 Hz), one zero, and a
                // resonance pair at 107.7 and 737.9 Hz. Written in f^2 because the whole response is
                // even in f, which is why the numerator carries f^4.
                double f2 = f * f;
                double num = (12194.0 * 12194.0) * (f2 * f2);
                double den = (f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0);
                w = (num / std::max(1e-12, den)) * inv_ref;
            } else if (weighting_code == 2) {
                // C-weighting: the same shape with the 107.7/737.9 resonance pair removed.
                double f2 = f * f;
                double num = (12194.0 * 12194.0) * f2;
                double den = (f2 + 20.6 * 20.6) * (f2 + 12194.0 * 12194.0);
                w = (num / std::max(1e-12, den)) * inv_ref;
            } else if (weighting_code == 3) {
                // ITU-R 468. Unlike A and C this is given by the standard as a fitted polynomial
                // pair (h1, h2) rather than a physical filter, so the coefficients are quoted
                // verbatim and must not be re-derived or rounded. The result is |H|, hence the
                // sqrt of the sum of squares.
                double f2 = f * f, f3 = f2 * f, f4 = f2 * f2, f5 = f4 * f, f6 = f3 * f3;
                double h1 = -4.737338981378384e-24 * f6 + 2.043828333266122e-15 * f4 - 1.363894795463638e-7 * f2 + 1.0;
                double h2 = 1.306612257412824e-19 * f5 - 2.118150887518656e-11 * f3 + 5.559488023498642e-4 * f;
                w = (1.246332637532143e-4 * f) / std::sqrt(std::max(1e-12, h1 * h1 + h2 * h2));
            }
            weights[i] = static_cast<float>(w);
        }
    }
};

/*
===========================================================================
 6a. FAST 20*log10 (IEEE 754 exponent + 2048-entry mantissa LUT, single gather)
===========================================================================
Max error 0.0021 dB: the table is sampled at mid-interval (see Table below), so the worst case is
half the 0.0042 dB entry spacing (= 20*log10(1 + 1/2048), what a table rounded to its lower edge
would give). The earlier 256-entry table was 0.034 dB.
One gather per 8 bins: an interpolated two-gather variant measured 2x slower in the
dB stage for no visible benefit on a display spectrum. The 8 KB table stays L1-resident.
Table is a function-local static -> thread-safe initialisation.

WHAT IT IS: 20*log10(x), computed without calling log10. A float is (exponent, mantissa), and
log10 factors into those two parts:

    20*log10(x) = 20*exponent*log10(2) + 20*log10(1 + mantissa)

The first term is one multiply (the exponent field is right there in the bits, and 20*log10(2) is
kLog10_2_Scaled). The second term depends only on the mantissa, which is in [1, 2), so it is
tabulated: the top kTableBits bits of the mantissa index the table. No branch, no convergence loop,
one multiply and one table read per value - which is what makes a 2048-bin dB conversion affordable
on the cook thread.

PRECONDITION: x must be a positive normal float. Zero, a denormal, a negative value, inf or NaN
produce a meaningless number rather than crashing, because the bit trick reads whatever the exponent
field happens to be (a zero's exponent field reads as -127, so the answer is ~-764 dB). Callers
rely on this only after flooring: DecibelConverter applies its dB floor to the result, and every
magnitude reaching it comes from |FFT output|^2 or |FFT output|, neither of which is negative. If
you ever feed it a value that can be negative, take the absolute value first - do not add a sign
test inside scaled(), that would put a branch in the hottest loop in the file.

HOW TO CHANGE: kTableBits is the only knob. It costs 2^bits * 4 bytes (11 -> 8 KB, the current
choice) and buys accuracy at half a bucket, so 12 bits halves the error for another 8 KB of L1 that
the rest of this pipeline wants. Leave it at 11 unless a measurement says otherwise.
*/
class FastLog10 {
public:
    static constexpr float kLog10_2_Scaled = 6.020599913282299f; // 20 * log10(2)
    static constexpr int kTableBits = 11;
    static constexpr int kTableSize = 1 << kTableBits;             // 2048 entries (8 KB)

    // The table itself. Build once, then pass the pointer around: this is why every entry point
    // below has an overload taking `const float* t`. See the precondition note above before use.
    static const float* dbTable() noexcept {
        static const struct Table {
            float v[kTableSize];
            Table() {
                for (int i = 0; i < kTableSize; ++i)
                    // Mid-interval: the entry for a mantissa bucket is evaluated at the bucket's centre,
                    // so the worst-case deviation is half a bucket instead of a whole one.
                    v[i] = 20.0f * std::log10(1.0f + (static_cast<float>(i) + 0.5f) / static_cast<float>(kTableSize));
            }
        } table;
        return table.v;
    }

    // Scalar 20*log10(x) for positive normal x. Pass the table pointer from dbTable() when calling in a loop.
    static float scaled(float x, const float* t) noexcept {
        uint32_t bits;
        std::memcpy(&bits, &x, sizeof(bits));
        int exp = static_cast<int>((bits >> 23) & 0xFF) - 127;                     // unbiased exponent
        int idx = static_cast<int>((bits >> (23 - kTableBits)) & (kTableSize - 1)); // top mantissa bits
        return exp * kLog10_2_Scaled + t[idx];
    }
    // Convenience overload for one-off calls; in a loop use the two-argument form so the
    // static-initialization guard is checked once instead of once per sample.
    static float scaled(float x) noexcept { return scaled(x, dbTable()); }

#if defined(__AVX2__)
    // Same computation, eight lanes at a time: exponent extraction and mantissa indexing are both
    // pure integer ops, and the gather is the only memory access. Returns 20*log10 for each lane.
    // PRECONDITION: every lane is a positive float (both callers clamp to >= 1e-12 first), so the sign
    // bit is 0 and bits >> 23 is the biased exponent already - the scalar form's & 0xFF is not needed.
    // (A polynomial log2 was measured as the alternative: 30 % slower than this gather on Raptor Lake.)
    static inline __m256 scaledVec(__m256 v, const float* t) noexcept {
        const __m256i bits = _mm256_castps_si256(v);
        const __m256i exp = _mm256_sub_epi32(_mm256_srli_epi32(bits, 23), _mm256_set1_epi32(127));
        const __m256i idx = _mm256_and_si256(_mm256_srli_epi32(bits, 23 - kTableBits), _mm256_set1_epi32(kTableSize - 1));
        const __m256 lut = _mm256_i32gather_ps(t, idx, 4);
        return _mm256_fmadd_ps(_mm256_cvtepi32_ps(exp), _mm256_set1_ps(kLog10_2_Scaled), lut);
    }
    static inline __m256 scaledVec(__m256 v) noexcept { return scaledVec(v, dbTable()); }
#endif
};

/*
===========================================================================
 6b. SIMD HELPERS (window, weighting, peak)
===========================================================================
Four small array kernels used by the magnitude and display stages: multiply a window into the time
block, multiply a weighting curve into the magnitude bins, and find the peak (with or without its
bin index). Each is written as "vector body if AVX2 is available, scalar tail for the remainder",
and the tail always produces the same result as the vector body would.

ALIGNMENT IS THE THING TO GET RIGHT. Every `_mm256_load_ps` here (as opposed to `loadu`) requires a
32-byte-aligned pointer, and these functions are called on the magnitude and window buffers that
AlignedVector allocated - see AlignedAllocator at the top of this file, which is why the default
alignment is 32 and not 16. Passing a plain std::vector<float>::data() or a subrange that is not a
multiple of 8 floats from the start of an aligned buffer will fault, not just run slowly. The
store side uses `storeu` everywhere, so an unaligned *output* is fine; it is the inputs that must
be aligned.

WHY THESE ARE FREE FUNCTIONS AND NOT MEMBERS: they take raw pointers and do one loop, so they can
be called on any buffer (a window, a spectrum, a scratch copy) without the caller owning an object
of the right type. Nothing here holds state between calls.
*/
// dst[i] = a[i] * b[i]. a and b must be 32-byte aligned; dst may be unaligned.
// Two vector loads and one store per 8 floats; the 16-wide loop above it is not four-way
// unrolling for its own sake but a hint that this is the same loop the compiler would otherwise
// have to prove non-aliasing for - hence the __restrict on all three pointers.
inline void multiplyInto(const float* __restrict a, const float* __restrict b, float* __restrict dst, size_t len) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 15 < len; i += 16) {
        _mm256_storeu_ps(dst + i,     _mm256_mul_ps(_mm256_load_ps(a + i),     _mm256_load_ps(b + i)));
        _mm256_storeu_ps(dst + i + 8, _mm256_mul_ps(_mm256_load_ps(a + i + 8), _mm256_load_ps(b + i + 8)));
    }
    for (; i + 7 < len; i += 8) {
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i)));
    }
#endif
    for (; i < len; ++i) dst[i] = a[i] * b[i];
}

// spectrum[i] *= curve[i] (both 32-byte aligned)
// The in-place twin of multiplyInto, used for the weighting stage: the weighting curve is applied
// to the spectrum buffer itself rather than to a copy, which is why the load and store are the same
// address and why this one must not be called with a curve shorter than the spectrum - len is the
// caller's promise that both are at least that long.
inline void multiplyInPlace(float* __restrict spectrum, const float* __restrict curve, size_t len) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 15 < len; i += 16) {
        _mm256_store_ps(spectrum + i,     _mm256_mul_ps(_mm256_load_ps(spectrum + i),     _mm256_load_ps(curve + i)));
        _mm256_store_ps(spectrum + i + 8, _mm256_mul_ps(_mm256_load_ps(spectrum + i + 8), _mm256_load_ps(curve + i + 8)));
    }
    for (; i + 7 < len; i += 8) {
        _mm256_store_ps(spectrum + i, _mm256_mul_ps(_mm256_load_ps(spectrum + i), _mm256_load_ps(curve + i)));
    }
#endif
    for (; i < len; ++i) spectrum[i] *= curve[i];
}

// spectrum[i] *= curve[i], returning max(spectrum) after the multiply: the weighting pass and the dB
// reference peak in one read of the spectrum (v2.12; used when a Frame Peak / AGC reference needs it).
inline float multiplyInPlaceMax(float* __restrict spectrum, const float* __restrict curve, size_t len) noexcept {
    float m = 0.0f;
    size_t i = 0;
#if defined(__AVX2__)
    __m256 vm0 = _mm256_setzero_ps(), vm1 = vm0, vm2 = vm0, vm3 = vm0;   // four chains: see peakMagnitude
    for (; i + 31 < len; i += 32) {
        const __m256 a = _mm256_mul_ps(_mm256_load_ps(spectrum + i), _mm256_load_ps(curve + i));
        const __m256 b = _mm256_mul_ps(_mm256_load_ps(spectrum + i + 8), _mm256_load_ps(curve + i + 8));
        const __m256 c = _mm256_mul_ps(_mm256_load_ps(spectrum + i + 16), _mm256_load_ps(curve + i + 16));
        const __m256 d = _mm256_mul_ps(_mm256_load_ps(spectrum + i + 24), _mm256_load_ps(curve + i + 24));
        _mm256_store_ps(spectrum + i, a); _mm256_store_ps(spectrum + i + 8, b);
        _mm256_store_ps(spectrum + i + 16, c); _mm256_store_ps(spectrum + i + 24, d);
        vm0 = _mm256_max_ps(vm0, a); vm1 = _mm256_max_ps(vm1, b); vm2 = _mm256_max_ps(vm2, c); vm3 = _mm256_max_ps(vm3, d);
    }
    for (; i + 7 < len; i += 8) {
        const __m256 a = _mm256_mul_ps(_mm256_load_ps(spectrum + i), _mm256_load_ps(curve + i));
        _mm256_store_ps(spectrum + i, a);
        vm0 = _mm256_max_ps(vm0, a);
    }
    alignas(32) float t[8];
    _mm256_store_ps(t, _mm256_max_ps(_mm256_max_ps(vm0, vm1), _mm256_max_ps(vm2, vm3)));
    for (int k = 0; k < 8; ++k) m = std::max(m, t[k]);
#endif
    for (; i < len; ++i) { spectrum[i] *= curve[i]; m = std::max(m, spectrum[i]); }
    return m;
}

// Maximum value (data 32-byte aligned)
// The horizontal reduction at the end (store 8 lanes to memory, scan them) is the standard way to
// finish a vector max; there is no cheaper instruction for it on AVX2. Called on the magnitude
// spectrum to find the value the dB reference is taken from (AbsFS/FramePeak modes) and to drive
// the AGC follower. Returns 0 for an empty range so a zero-length axis cannot poison the reference.
//
// v2.12: four independent accumulators. With one, every iteration waited for the previous max
// (4-cycle latency) and the loop was latency-bound: 0.51 us at 8193 bins. Four chains let the loads run
// at throughput.
inline float peakMagnitude(const float* __restrict data, size_t n) noexcept {
    if (n == 0) return 0.0f;
    float max_val = data[0];
    size_t i = 0;
#if defined(__AVX2__)
    __m256 v_max = _mm256_set1_ps(max_val), m1 = v_max, m2 = v_max, m3 = v_max;
    for (; i + 31 < n; i += 32) {
        v_max = _mm256_max_ps(v_max, _mm256_load_ps(data + i));
        m1 = _mm256_max_ps(m1, _mm256_load_ps(data + i + 8));
        m2 = _mm256_max_ps(m2, _mm256_load_ps(data + i + 16));
        m3 = _mm256_max_ps(m3, _mm256_load_ps(data + i + 24));
    }
    v_max = _mm256_max_ps(_mm256_max_ps(v_max, m1), _mm256_max_ps(m2, m3));
    for (; i + 7 < n; i += 8) v_max = _mm256_max_ps(v_max, _mm256_load_ps(data + i));
    alignas(32) float tmp[8];
    _mm256_store_ps(tmp, v_max);
    for (int k = 0; k < 8; ++k) max_val = std::max(max_val, tmp[k]);
#endif
    for (; i < n; ++i) max_val = std::max(max_val, data[i]);
    return max_val;
}

// Peak value and index (data 32-byte aligned). The first bin wins a tie (strict >), which is what
// makes the reported peak index stable frame to frame when two bins hold the same value.
//
// HOW: four independent (running max, index-of-max) vector pairs over 32 bins per step. Per lane a
// strict > keeps the earliest index, and the final 32-way reduction breaks value ties on the lower
// index, so the result is exactly the scalar "first maximum". No data-dependent branch and no
// loop-carried latency chain (four chains hide the compare+blend latency), so the cost no longer
// depends on the data: the previous "filter, then rescan the chunk" form cost 0.85 us on a noisy
// spectrum but 4.5 us on a rising one (every chunk a new maximum); this is ~0.9 us for both at 16384
// bins (i9-13900H P-core). peak_idx is always written (0 for an empty range).
inline float findPeakWithIndex(const float* __restrict data, size_t n, size_t& peak_idx) noexcept {
    peak_idx = 0;
    if (n == 0) return 0.0f;
    float max_val = data[0];
    size_t i = 0;
#if defined(__AVX2__)
    if (n >= 32) {
        __m256 m0 = _mm256_load_ps(data), m1 = _mm256_load_ps(data + 8), m2 = _mm256_load_ps(data + 16), m3 = _mm256_load_ps(data + 24);
        const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        __m256i c0 = lane, c1 = _mm256_add_epi32(lane, _mm256_set1_epi32(8)),
                c2 = _mm256_add_epi32(lane, _mm256_set1_epi32(16)), c3 = _mm256_add_epi32(lane, _mm256_set1_epi32(24));
        __m256i x0 = c0, x1 = c1, x2 = c2, x3 = c3;
        const __m256i step = _mm256_set1_epi32(32);
        auto upd = [](__m256& m, __m256i& x, __m256 v, __m256i c) noexcept {
            const __m256 gt = _mm256_cmp_ps(v, m, _CMP_GT_OQ);
            m = _mm256_max_ps(m, v);
            x = _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(x), _mm256_castsi256_ps(c), gt));
        };
        for (i = 32; i + 31 < n; i += 32) {
            c0 = _mm256_add_epi32(c0, step); c1 = _mm256_add_epi32(c1, step);
            c2 = _mm256_add_epi32(c2, step); c3 = _mm256_add_epi32(c3, step);
            upd(m0, x0, _mm256_load_ps(data + i), c0);
            upd(m1, x1, _mm256_load_ps(data + i + 8), c1);
            upd(m2, x2, _mm256_load_ps(data + i + 16), c2);
            upd(m3, x3, _mm256_load_ps(data + i + 24), c3);
        }
        alignas(32) float mv[32];
        alignas(32) int32_t iv[32];
        _mm256_store_ps(mv, m0); _mm256_store_ps(mv + 8, m1); _mm256_store_ps(mv + 16, m2); _mm256_store_ps(mv + 24, m3);
        _mm256_store_si256(reinterpret_cast<__m256i*>(iv), x0);      _mm256_store_si256(reinterpret_cast<__m256i*>(iv + 8), x1);
        _mm256_store_si256(reinterpret_cast<__m256i*>(iv + 16), x2); _mm256_store_si256(reinterpret_cast<__m256i*>(iv + 24), x3);
        max_val = mv[0]; peak_idx = static_cast<size_t>(iv[0]);
        for (int l = 1; l < 32; ++l) {
            const size_t li = static_cast<size_t>(iv[l]);
            if (mv[l] > max_val || (mv[l] == max_val && li < peak_idx)) { max_val = mv[l]; peak_idx = li; }
        }
    }
#endif
    for (; i < n; ++i) {
        if (data[i] > max_val) { max_val = data[i]; peak_idx = i; }
    }
    return max_val;
}

/*
===========================================================================
 6b'. FastLog2Seg - log2 without a table gather (v2.12, used by the dB stage)
===========================================================================
WHAT: log2(x) for positive normal floats as exponent + an 8-segment quadratic of the mantissa. The
      segment is the top 3 mantissa bits, and its three coefficients are fetched with vpermps from
      three registers - no memory access at all (FastLog10's 8 KB table needs a hardware gather).
ACCURACY: max |error| 2.7e-5 in log2 = 0.00016 dB (Chebyshev fit per segment); the table is 0.0027 dB.
SPEED (i9-13900H, 16384 bins, min of 60 rounds x 3 reps, interleaved): dB-normalized 15 % faster than
      the gather table, plain dB equal. Gathers are also the slow instruction on E-cores, which this
      avoids. Coefficients: per segment j, the quadratic through log2(1 + j/8 + u) at the 3 Chebyshev
      nodes of u in [0, 1/8), solved in long double (refit = same recipe; the test pins the error).
PRECONDITION: x > 0 and normal (the dB stage clamps to >= 1e-12 first).
*/
struct FastLog2Seg {
    // c0, c1, c2 per segment j = top 3 mantissa bits; u = mantissa - j/8 in [0, 1/8).
    alignas(32) static constexpr float kC0[8] = { 2.564386887e-05f, 1.699432731e-01f, 3.219415843e-01f, 4.594418406e-01f,
                                                  5.849704146e-01f, 7.004460096e-01f, 8.073599935e-01f, 9.068947434e-01f };
    alignas(32) static constexpr float kC1[8] = { 1.438983321e+00f, 1.279752135e+00f, 1.152207255e+00f, 1.047755003e+00f,
                                                  9.606496096e-01f, 8.869041204e-01f, 8.236659169e-01f, 7.688398957e-01f };
    alignas(32) static constexpr float kC2[8] = { -6.398096681e-01f, -5.120694041e-01f, -4.190979004e-01f, -3.493308127e-01f,
                                                  -2.956413627e-01f, -2.534430921e-01f, -2.196758091e-01f, -1.922342032e-01f };
    static constexpr float kDbPerOctave = 6.0205999132796239f;   // 20 * log10(2)

    static float log2(float x) noexcept {
        uint32_t bits;
        std::memcpy(&bits, &x, sizeof(bits));
        const uint32_t j = (bits >> 20) & 7u;
        const float e = static_cast<float>(static_cast<int>(bits >> 23) - 127);
        const uint32_t ub = (bits & 0x000FFFFFu) | 0x3F800000u;
        float u;
        std::memcpy(&u, &ub, sizeof(u));
        u -= 1.0f;
        return (kC0[j] + u * (kC1[j] + u * kC2[j])) + e;
    }
#if defined(__AVX2__)
    struct Regs { __m256 c0, c1, c2; };
    static Regs regs() noexcept { return { _mm256_load_ps(kC0), _mm256_load_ps(kC1), _mm256_load_ps(kC2) }; }
    static inline __m256 log2(__m256 v, const Regs& r) noexcept {
        const __m256i bits = _mm256_castps_si256(v);
        const __m256i j = _mm256_srli_epi32(bits, 20);            // vpermps reads only the low 3 bits: the segment
        const __m256 e = _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_srli_epi32(bits, 23), _mm256_set1_epi32(127)));
        const __m256 u = _mm256_sub_ps(_mm256_castsi256_ps(_mm256_or_si256(_mm256_and_si256(bits, _mm256_set1_epi32(0x000FFFFF)),
                                                                           _mm256_set1_epi32(0x3F800000))), _mm256_set1_ps(1.0f));
        __m256 q = _mm256_fmadd_ps(_mm256_permutevar8x32_ps(r.c2, j), u, _mm256_permutevar8x32_ps(r.c1, j));
        q = _mm256_fmadd_ps(q, u, _mm256_permutevar8x32_ps(r.c0, j));
        return _mm256_add_ps(q, e);
    }
#endif
};

/*
===========================================================================
 6c. DECIBEL CONVERSION
===========================================================================
mode: 1 = dB, 2 = dB normalised to [0,1] over [-top_db, 0]
inv_ref: 1 / reference magnitude (0 dB point). The caller chooses the reference
(frame peak, absolute full scale, or an AGC follower).
*/
class DecibelConverter {
public:
    // v2.12: log2 by FastLog2Seg (no gather, 0.00016 dB) and every constant folded into one FMA:
    //   dB   = log2(m) * 6.0206 + 20*log10(inv_ref)                      , then max(dB, -top_db)
    //   norm = log2(m) * (6.0206 / top_db) + (20*log10(inv_ref) / top_db + 1), then clamp [0, 1]
    // (norm = (dB - floor) / top_db with floor = -top_db, and dB >= floor is exactly norm >= 0.)
    // The reference offset is computed exactly once per call with std::log10.
    static inline void convertToDB(int mode, double top_db, float inv_ref, AlignedVector& spectrum) noexcept {
        if (mode == 0 || spectrum.empty()) return;
        size_t n = spectrum.size();
        float* data = spectrum.data();
        if (!(inv_ref > 0.0f) || !std::isfinite(inv_ref)) inv_ref = 1.0f;

        const double tdb = std::max(1e-6, top_db);
        const float db_offset = static_cast<float>(20.0 * std::log10(static_cast<double>(inv_ref)));
        const float floor_val = static_cast<float>(-top_db);
        const float kMinMag = 1e-12f;
        const float k1 = FastLog2Seg::kDbPerOctave;
        const float k2 = static_cast<float>(FastLog2Seg::kDbPerOctave / tdb);
        const float c2 = static_cast<float>(db_offset / tdb + 1.0);

        size_t k = 0;
#if defined(__AVX2__)
        const FastLog2Seg::Regs lr = FastLog2Seg::regs();
        const __m256 min_v = _mm256_set1_ps(kMinMag);
        if (mode == 2) {
            const __m256 zero_v = _mm256_setzero_ps(), one_v = _mm256_set1_ps(1.0f);
            const __m256 kv = _mm256_set1_ps(k2), cv = _mm256_set1_ps(c2);
            for (; k + 15 < n; k += 16) {
                const __m256 l0 = FastLog2Seg::log2(_mm256_max_ps(_mm256_load_ps(data + k), min_v), lr);
                const __m256 l1 = FastLog2Seg::log2(_mm256_max_ps(_mm256_load_ps(data + k + 8), min_v), lr);
                _mm256_store_ps(data + k,     _mm256_min_ps(_mm256_max_ps(_mm256_fmadd_ps(l0, kv, cv), zero_v), one_v));
                _mm256_store_ps(data + k + 8, _mm256_min_ps(_mm256_max_ps(_mm256_fmadd_ps(l1, kv, cv), zero_v), one_v));
            }
            for (; k + 7 < n; k += 8) {
                const __m256 l = FastLog2Seg::log2(_mm256_max_ps(_mm256_load_ps(data + k), min_v), lr);
                _mm256_store_ps(data + k, _mm256_min_ps(_mm256_max_ps(_mm256_fmadd_ps(l, kv, cv), zero_v), one_v));
            }
        } else {
            const __m256 kv = _mm256_set1_ps(k1), cv = _mm256_set1_ps(db_offset), floor_v = _mm256_set1_ps(floor_val);
            for (; k + 15 < n; k += 16) {
                const __m256 l0 = FastLog2Seg::log2(_mm256_max_ps(_mm256_load_ps(data + k), min_v), lr);
                const __m256 l1 = FastLog2Seg::log2(_mm256_max_ps(_mm256_load_ps(data + k + 8), min_v), lr);
                _mm256_store_ps(data + k,     _mm256_max_ps(_mm256_fmadd_ps(l0, kv, cv), floor_v));
                _mm256_store_ps(data + k + 8, _mm256_max_ps(_mm256_fmadd_ps(l1, kv, cv), floor_v));
            }
            for (; k + 7 < n; k += 8) {
                const __m256 l = FastLog2Seg::log2(_mm256_max_ps(_mm256_load_ps(data + k), min_v), lr);
                _mm256_store_ps(data + k, _mm256_max_ps(_mm256_fmadd_ps(l, kv, cv), floor_v));
            }
        }
#endif
        for (; k < n; ++k) {
            const float l = FastLog2Seg::log2(std::max(kMinMag, data[k]));
            data[k] = (mode == 2) ? std::max(0.0f, std::min(1.0f, l * k2 + c2)) : std::max(l * k1 + db_offset, floor_val);
        }
    }
};

/*
===========================================================================
 6d. SPECTRAL FEATURES (v2.10, optional: "Spectral Features" toggle)
===========================================================================
WHAT: a handful of scalar descriptors of one frame, computed on the analysis worker from the LINEAR
      magnitude spectrum (before the warp, so they do not depend on the display axis) and published on
      the Info CHOP. These are what audio-reactive visuals are usually driven by; computing them here
      costs one pass over the magnitude bins instead of a downstream CHOP network over the full output.

  centroidHz   power-weighted mean frequency ("brightness")
  rolloffHz    frequency below which 85 % of the power lies
  flatness     geometric / arithmetic mean of the power, 0 (tonal) .. 1 (noise)
  flux         half-wave rectified frame-to-frame magnitude increase, normalised by the frame's total
               magnitude: an onset-strength signal (~0 steady, spikes on attacks)
  rmsDb        RMS of the analysis window's time samples, dBFS (independent of the window/normalisation)
  bass/mid/highDb  10*log10 of the power in < 250 Hz, 250 Hz - 4 kHz, > 4 kHz, on the spectrum's own
               magnitude scale (compare them with each other and over time, not as absolute levels)

COST: O(n) with n = the magnitude bins computed (up to Display Max); ~3.5 us at 8193 bins (i9-13900H,
measured; the pre-2.12 scalar loop was ~16-20 us, not the 2-4 us this line used to claim).
*/
struct SpectralFeatures {
    float centroidHz{ 0.0f }, rolloffHz{ 0.0f }, flatness{ 0.0f }, flux{ 0.0f };
    float rmsDb{ -120.0f }, bassDb{ -120.0f }, midDb{ -120.0f }, highDb{ -120.0f };
};

namespace detail {
#if defined(__AVX2__)
inline float hsum8(__m256 v) noexcept {
    __m128 s4 = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    s4 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
    return _mm_cvtss_f32(_mm_add_ss(s4, _mm_shuffle_ps(s4, s4, 1)));
}
#endif
// The per-bin sums of computeSpectralFeatures over [lo, hi). One call per band, so the band test is
// gone from the loop. The vector body accumulates in float lanes and flushes to double every 256
// bins, which keeps a 32K-bin sum at ~1e-7 relative accuracy (the result matches the double loop to
// the printed precision of every feature).
struct FeatureSums { double pw = 0.0, weighted = 0.0, mag = 0.0, logDb = 0.0, rise = 0.0; };
// logDb accumulates 20*log10(m) with the FastLog10 table: in this loop, already ALU-heavy with five
// accumulations, the gather (load ports) measured 4 % faster than FastLog2Seg (ALU); the dB stage is the
// opposite case.
inline void featureSums(const float* mag, const float* prev, size_t lo, size_t hi, FeatureSums& a) noexcept {
    size_t k = lo;
    const float* lut = FastLog10::dbTable();
#if defined(__AVX2__)
    const __m256 tiny = _mm256_set1_ps(1e-12f), z = _mm256_setzero_ps(), eight = _mm256_set1_ps(8.0f);
    while (k + 8 <= hi) {
        const size_t end = std::min(hi, k + 256);
        __m256 spw = z, swk = z, smag = z, slg = z, srise = z;
        __m256 kk = _mm256_add_ps(_mm256_set1_ps(static_cast<float>(k)), _mm256_setr_ps(0, 1, 2, 3, 4, 5, 6, 7));
        for (; k + 8 <= end; k += 8) {
            const __m256 m = _mm256_loadu_ps(mag + k);
            const __m256 pw = _mm256_mul_ps(m, m);
            spw = _mm256_add_ps(spw, pw);
            swk = _mm256_fmadd_ps(pw, kk, swk);
            smag = _mm256_add_ps(smag, m);
            slg = _mm256_add_ps(slg, FastLog10::scaledVec(_mm256_max_ps(m, tiny), lut));   // 20*log10(m)
            if (prev) srise = _mm256_add_ps(srise, _mm256_max_ps(_mm256_sub_ps(m, _mm256_loadu_ps(prev + k)), z));
            kk = _mm256_add_ps(kk, eight);
        }
        a.pw += hsum8(spw); a.weighted += hsum8(swk); a.mag += hsum8(smag); a.logDb += hsum8(slg); a.rise += hsum8(srise);
    }
#endif
    for (; k < hi; ++k) {
        const float m = mag[k];
        const double pw = static_cast<double>(m) * m;
        a.pw += pw;
        a.weighted += pw * static_cast<double>(k);
        a.mag += m;
        a.logDb += FastLog10::scaled(std::max(m, 1e-12f), lut);
        if (prev) { const float d = m - prev[k]; if (d > 0.0f) a.rise += d; }
    }
}
} // namespace detail

// mag/n: linear magnitude (bin k is at k*binHz). time/nTime: the analysis window's raw samples (for
// rmsDb). prev: the previous frame's magnitude (flux state, resized/overwritten here - per channel).
//
// v2.12: the loop is split at the two band edges (no per-bin band branch), the sums are AVX2
// (detail::featureSums), the rolloff search skips 64-bin blocks before scanning the one that crosses
// 85 %, and the time-domain RMS is vectorized. Measured 16.5 -> 3.5 us at 8193 bins + 3175 samples
// (i9-13900H P-core); the features are the same to the printed precision.
inline void computeSpectralFeatures(const float* mag, size_t n, double binHz, const float* time, size_t nTime,
                                    AlignedVector& prev, SpectralFeatures& f) noexcept
{
    f = SpectralFeatures{};
    if (n == 0 || binHz <= 0.0) return;
    const size_t b250 = std::min(n, static_cast<size_t>(250.0 / binHz) + 1);
    const size_t b4k = std::min(n, static_cast<size_t>(4000.0 / binHz) + 1);
    const float* pv = (prev.size() >= n) ? prev.data() : nullptr;
    detail::FeatureSums lo, mid, hi;
    detail::featureSums(mag, pv, 0, b250, lo);
    detail::featureSums(mag, pv, b250, b4k, mid);
    detail::featureSums(mag, pv, b4k, n, hi);
    const double total = lo.pw + mid.pw + hi.pw;
    const double sumMag = lo.mag + mid.mag + hi.mag;
    if (total > 0.0) {
        f.centroidHz = static_cast<float>((lo.weighted + mid.weighted + hi.weighted) / total * binHz);
        const double target = 0.85 * total;
        double acc = 0.0;
        size_t k = 0;
#if defined(__AVX2__)
        for (; k + 64 <= n; k += 64) {                     // whole 64-bin blocks below the 85 % point
            // four FMA chains (one chain was latency-bound: 8 dependent FMAs per block)
            __m256 s0 = _mm256_setzero_ps(), s1 = s0, s2 = s0, s3 = s0;
            for (size_t j = 0; j < 64; j += 32) {
                const __m256 a0 = _mm256_loadu_ps(mag + k + j), a1 = _mm256_loadu_ps(mag + k + j + 8);
                const __m256 a2 = _mm256_loadu_ps(mag + k + j + 16), a3 = _mm256_loadu_ps(mag + k + j + 24);
                s0 = _mm256_fmadd_ps(a0, a0, s0); s1 = _mm256_fmadd_ps(a1, a1, s1);
                s2 = _mm256_fmadd_ps(a2, a2, s2); s3 = _mm256_fmadd_ps(a3, a3, s3);
            }
            const double b = detail::hsum8(_mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3)));
            if (acc + b >= target) break;
            acc += b;
        }
#endif
        for (; k < n; ++k) { acc += static_cast<double>(mag[k]) * mag[k]; if (acc >= target) break; }
        f.rolloffHz = static_cast<float>(std::min(k, n - 1) * binHz);
        const double logSum = lo.logDb + mid.logDb + hi.logDb;
        const double geo = std::pow(10.0, (logSum / static_cast<double>(n)) / 10.0);   // geometric mean of power
        f.flatness = static_cast<float>(std::clamp(geo / (total / static_cast<double>(n)), 0.0, 1.0));
    }
    auto db = [](double p) { return static_cast<float>(p > 1e-24 ? 10.0 * std::log10(p) : -240.0); };
    f.bassDb = db(lo.pw); f.midDb = db(mid.pw); f.highDb = db(hi.pw);
    f.flux = (pv && sumMag > 0.0) ? static_cast<float>((lo.rise + mid.rise + hi.rise) / sumMag) : 0.0f;
    if (time && nTime) {
        double s2 = 0.0;
        size_t i = 0;
#if defined(__AVX2__)
        while (i + 8 <= nTime) {
            const size_t end = std::min(nTime, i + 512);
            __m256 q0 = _mm256_setzero_ps(), q1 = q0, q2 = q0, q3 = q0;          // four FMA chains
            for (; i + 32 <= end; i += 32) {
                const __m256 x0 = _mm256_loadu_ps(time + i), x1 = _mm256_loadu_ps(time + i + 8);
                const __m256 x2 = _mm256_loadu_ps(time + i + 16), x3 = _mm256_loadu_ps(time + i + 24);
                q0 = _mm256_fmadd_ps(x0, x0, q0); q1 = _mm256_fmadd_ps(x1, x1, q1);
                q2 = _mm256_fmadd_ps(x2, x2, q2); q3 = _mm256_fmadd_ps(x3, x3, q3);
            }
            for (; i + 8 <= end; i += 8) { const __m256 x = _mm256_loadu_ps(time + i); q0 = _mm256_fmadd_ps(x, x, q0); }
            s2 += detail::hsum8(_mm256_add_ps(_mm256_add_ps(q0, q1), _mm256_add_ps(q2, q3)));
        }
#endif
        for (; i < nTime; ++i) s2 += static_cast<double>(time[i]) * time[i];
        const double rms = std::sqrt(s2 / static_cast<double>(nTime));
        f.rmsDb = static_cast<float>(rms > 1e-6 ? 20.0 * std::log10(rms) : -120.0);
    }
    if (prev.size() != n) prev.resize(n);                                   // grows once, then reused
    std::memcpy(prev.data(), mag, n * sizeof(float));
}

/*
===========================================================================
 7. ASYMMETRIC ATTACK / RELEASE BALLISTICS FILTER
===========================================================================
attack/release are per-frame smoothing coefficients in [0, 0.99]
(0 = follow instantly). Use coefFromMs() for frame-rate independent values.

WHAT IT DOES: smooths a spectrum over time - a bin that jumps up follows the attack coefficient,
a bin that falls follows the release one, which is what makes a display spectrum rise quickly but
decay gracefully instead of flickering bin to bin. It is the only stage in the pipeline that keeps
per-bin state across cooks (prev_out doubles as that state), which is why the pipeline has to
zero it when the node is silent or the mode changes - see runChannel() in AnalysisPipeline.cpp.

HOW TO READ THE COEFFICIENTS: they are "how much of the previous frame to keep", so the code below
stores them as 1 - coefficient because the update is written as old + factor*(new - old). A value of
0 means "follow the new value instantly"; larger means slower. Do not change the 0.99 clamp: it is
what stops a long time constant from making the state effectively permanent (and from a dt of 0
producing a coefficient of exactly 1, which would freeze the display forever).

HOW TO CHANGE: nothing here needs touching to add a parameter - the attack/release values come from
Parameters, and coefFromMs is the only intended way to turn a user-facing time in ms into these.
*/
class BallisticsFilter {
public:
    // Time constant (ms) -> per-frame coefficient for the given frame delta (ms). tau = time to reach 63%.
    // Frame-rate independence lives here: exp(-dt/tau) gives the same decay per millisecond whether
    // the node cooks at 30 or 120 fps, so a user-set 200 ms release means 200 ms at any frame rate.
    // A non-positive time or delta returns 0 (no smoothing) rather than dividing by zero.
    static float coefFromMs(double time_ms, double dt_ms) noexcept {
        if (time_ms <= 0.0 || dt_ms <= 0.0) return 0.0f;
        return static_cast<float>(std::clamp(std::exp(-dt_ms / time_ms), 0.0, 0.999));
    }

    // One frame of smoothing: prev_out is both the previous state and the output.
    // The first line handles the two cases where there is nothing to smooth from: a size change
    // (the state belongs to a different axis, so it is discarded and restarted at the new value)
    // and both coefficients zero (the caller turned the filter off, so the copy is the result).
    // Note this function only makes sense with both arrays the same length and 32-byte aligned.
    inline void apply(float attack, float release, const AlignedVector& current, AlignedVector& prev_out) noexcept {
        size_t n = current.size();
        if (prev_out.size() != n || (attack <= 0.0f && release <= 0.0f)) { prev_out = current; return; }
        float att_factor = 1.0f - std::clamp(attack, 0.0f, 0.999f);
        float rel_factor = 1.0f - std::clamp(release, 0.0f, 0.999f);
        const float* src = current.data();
        float* dst = prev_out.data();   // load *and* store target: this is the carried state
        size_t i = 0;
#if defined(__AVX2__)
        // The per-bin attack-or-release choice is a branch in the scalar code and would be a
        // mispredicting one in SIMD (adjacent bins routinely go opposite ways), so the vector body
        // selects the coefficient with blendv on a sign-agnostic "is the new value higher" compare
        // instead. Same arithmetic, no branch. The scalar tail below keeps the if/else form.
        // (Comparing s > d instead of diff > 0 shortens each vector's dependency chain, but the
        // iterations are independent, so it is throughput- and bandwidth-bound: measured no change,
        // 1013 vs 1027 ns at 16384 bins. Not worth a second spelling of the same test.)
        const __m256 v_att = _mm256_set1_ps(att_factor);
        const __m256 v_rel = _mm256_set1_ps(rel_factor);
        const __m256 v_zero = _mm256_setzero_ps();
        for (; i + 15 < n; i += 16) {
            __m256 s0 = _mm256_load_ps(src + i),     d0 = _mm256_load_ps(dst + i);
            __m256 s1 = _mm256_load_ps(src + i + 8), d1 = _mm256_load_ps(dst + i + 8);
            __m256 diff0 = _mm256_sub_ps(s0, d0), diff1 = _mm256_sub_ps(s1, d1);
            __m256 f0 = _mm256_blendv_ps(v_rel, v_att, _mm256_cmp_ps(diff0, v_zero, _CMP_GT_OQ));
            __m256 f1 = _mm256_blendv_ps(v_rel, v_att, _mm256_cmp_ps(diff1, v_zero, _CMP_GT_OQ));
            _mm256_store_ps(dst + i,     _mm256_fmadd_ps(f0, diff0, d0));
            _mm256_store_ps(dst + i + 8, _mm256_fmadd_ps(f1, diff1, d1));
        }
        for (; i + 7 < n; i += 8) {
            __m256 s = _mm256_load_ps(src + i), d = _mm256_load_ps(dst + i);
            __m256 diff = _mm256_sub_ps(s, d);
            __m256 f = _mm256_blendv_ps(v_rel, v_att, _mm256_cmp_ps(diff, v_zero, _CMP_GT_OQ));
            _mm256_store_ps(dst + i, _mm256_fmadd_ps(f, diff, d));
        }
#endif
        // Scalar tail - also the whole implementation on a non-AVX2 build. Note that a diff of
        // exactly 0 takes the release branch, matching the vector body's strict > comparison.
        for (; i < n; ++i) {
            float diff = src[i] - dst[i];
            dst[i] += ((diff > 0.0f) ? att_factor : rel_factor) * diff;
        }
    }

    // The pipeline's form: `io` is the new frame on entry and the smoothed frame on exit, and `state`
    // (the carried history) receives the same smoothed frame - both written in the one pass. This is
    // apply() followed by copying state into io, without the separate 16384-float copy pass
    // (v2.12: the runChannel memcpy it replaces was a full extra load+store of the spectrum per cook).
    inline void applyInPlace(float attack, float release, AlignedVector& io, AlignedVector& state) noexcept {
        const size_t n = io.size();
        if (state.size() != n || (attack <= 0.0f && release <= 0.0f)) { state = io; return; }
        const float att_factor = 1.0f - std::clamp(attack, 0.0f, 0.999f);
        const float rel_factor = 1.0f - std::clamp(release, 0.0f, 0.999f);
        float* __restrict x = io.data();
        float* __restrict st = state.data();
        size_t i = 0;
#if defined(__AVX2__)
        const __m256 v_att = _mm256_set1_ps(att_factor), v_rel = _mm256_set1_ps(rel_factor), v_zero = _mm256_setzero_ps();
        for (; i + 15 < n; i += 16) {
            const __m256 s0 = _mm256_load_ps(x + i), d0 = _mm256_load_ps(st + i);
            const __m256 s1 = _mm256_load_ps(x + i + 8), d1 = _mm256_load_ps(st + i + 8);
            const __m256 diff0 = _mm256_sub_ps(s0, d0), diff1 = _mm256_sub_ps(s1, d1);
            const __m256 r0 = _mm256_fmadd_ps(_mm256_blendv_ps(v_rel, v_att, _mm256_cmp_ps(diff0, v_zero, _CMP_GT_OQ)), diff0, d0);
            const __m256 r1 = _mm256_fmadd_ps(_mm256_blendv_ps(v_rel, v_att, _mm256_cmp_ps(diff1, v_zero, _CMP_GT_OQ)), diff1, d1);
            _mm256_store_ps(st + i, r0); _mm256_store_ps(st + i + 8, r1);
            _mm256_store_ps(x + i, r0);  _mm256_store_ps(x + i + 8, r1);
        }
        for (; i + 7 < n; i += 8) {
            const __m256 s = _mm256_load_ps(x + i), d = _mm256_load_ps(st + i);
            const __m256 diff = _mm256_sub_ps(s, d);
            const __m256 r = _mm256_fmadd_ps(_mm256_blendv_ps(v_rel, v_att, _mm256_cmp_ps(diff, v_zero, _CMP_GT_OQ)), diff, d);
            _mm256_store_ps(st + i, r); _mm256_store_ps(x + i, r);
        }
#endif
        for (; i < n; ++i) {
            const float diff = x[i] - st[i];
            const float r = st[i] + ((diff > 0.0f) ? att_factor : rel_factor) * diff;
            st[i] = r; x[i] = r;
        }
    }
};

/*
===========================================================================
 8. FFT ENGINE INTERFACE & AVX2 MAGNITUDE
===========================================================================
Two things live here: the abstract interface the pipeline talks to (IFFTEngine), and the SIMD
kernel that turns FFTW's complex output into the magnitude bins everything downstream works on.

WHY AN INTERFACE AT ALL: the pipeline needs exactly four operations - build a plan, run a
transform, report what the plan is, and report whether a plan exists. Everything about *which*
library performs the transform (the vendored FFTW3 build, or Intel oneMKL's FFTW3-compatible
interface) sits behind this interface, so the pipeline never names either one and the backend
choice stays a runtime parameter. If a third backend is ever added, it is a new implementation of
this interface and nothing else in the plugin changes.

The four methods are not all pure virtual on purpose: pollBackgroundPlan, setBackgroundAllowed and
backendReport have safe no-op defaults, so an engine that has no background planning, no library
and no report does not have to write three empty overrides to compile.
*/

// How much effort the FFTW planner is allowed to spend, in increasing order of cost and speed.
// The UI dropdown maps onto these four (Parameters.h, FFT Plan) - keep the two in step.
//
// Auto is the default and the one to understand: it never makes the user wait. The first plan for a
// size is built cheaply (from wisdom if present, otherwise FFTW_ESTIMATE), the node starts running
// immediately, and a background thread then re-plans the same size with FFTW_MEASURE and swaps the
// better plan in when it is ready - see startBackgroundMeasure/pollBackgroundPlan below.
//
// Patient is the same shape with FFTW_PATIENT as the upgrade target: a few percent faster at
// execute time for seconds spent planning, once per size per machine (the result is written to
// wisdom, so it is a first-run cost only).
//
// Measured and Fast both block that background upgrade: Measured by doing the expensive plan
// synchronously up front, Fast by never planning expensively at all. Fast is what you want when
// the node must not stall under any circumstance and the transform is not the bottleneck.
//
// IF YOU ARE NOT SURE WHICH TO PICK: Auto. It is the only one that is both fast on the first frame
// and optimal on every frame after, and it degrades gracefully when Async is off - it reports the
// deferred upgrade in the plan status instead of silently measuring on the cook thread.
enum class PlannerPolicy : int {
    Auto = 0,     // instant plan (wisdom or ESTIMATE), FFTW_MEASURE upgraded on a background thread
    Fast = 1,     // ESTIMATE always
    Measured = 2, // MEASURE synchronously; plans are cached in wisdom so only the first run of a size costs time
    Patient = 3,  // like Auto but the background upgrade is FFTW_PATIENT (~10 % faster execute, seconds of planning once per size)
};

/*
The contract every FFT backend must satisfy. An implementation owns one plan for one FFT size for
one backend library; changing any of those means calling prepare() again.

THREADING CONTRACT (important, and the reason the signatures look the way they do):
  - prepare() is called from the cooking thread (or from the pipeline's rebuild path) and may
    block, because planning may be expensive. It is not callable concurrently with itself.
  - executeRFFT() is const and must be callable from the worker thread *concurrently with* a cook
    on the main thread, which is why it takes its scratch buffer as an argument instead of hiding
    one in the object, and why it is marked noexcept: an allocation or throw on the analysis thread
    would be unrecoverable there. All output buffers must be pre-sized by the caller for the same
    reason.
  - pollBackgroundPlan() and setBackgroundAllowed() are called once per cook, on the cooking thread,
    before any executeRFFT for that cook - so they never race with the background planner's own
    bookkeeping (the plan swap itself is what the engine locks internally).
*/
class IFFTEngine {
public:
    virtual ~IFFTEngine() = default;
    // Build (or rebuild) the plan for the given size and policy. `log` may be null; it is where plan
    // progress and the resulting plan's description are recorded for the Info DAT.
    // backend: which FFTW3-ABI library to plan against (see FftBackend.h). nullptr keeps whatever
    // backend is already selected, or the default when there is none. The library is loaded on first
    // use and kept for the process lifetime, so switching back and forth only re-plans.
    virtual void prepare(size_t fft_size, PlannerPolicy policy, PlanLog* log,
                         const FftBackendInfo* backend = nullptr) = 0;
    // Transform `padded_signal` (fft_size real samples, windowed, zero-padded) and write magnitudes.
    // n_mag: how many magnitude bins the caller will read (0 = all N/2+1). It is a lower bound on
    // what gets written, not an exact count: the AVX2 kernel works in 16-bin blocks, so an
    // implementation is free to produce up to 15 bins more than asked (see the FFTW override). The
    // magnitude_spectrum vector is always sized N/2+1 regardless.
    // scratch_complex is passed in, not owned, because the caller allocates it once outside the
    // real-time path - see the threading contract above.
    virtual void executeRFFT(const AlignedVector& padded_signal, AlignedVector& magnitude_spectrum,
                             AlignedComplexVector& scratch_complex, size_t n_mag = 0) const noexcept = 0;
    // Human-readable description of the live plan (which algorithm FFTW chose, whether it is
    // measured or estimated, its cost estimate). This is the Info DAT's plan line; it is built on
    // demand, never per cook.
    virtual std::string getPlanStatus() const = 0;
    virtual size_t fftSize() const noexcept = 0;
    // True if a valid plan is ready to execute (i.e. prepare() produced m_plan). Lets the pipeline
    // escalate a failed plan creation to getErrorString instead of silently outputting zeros.
    virtual bool hasPlan() const noexcept = 0;
    // Called once per cook on the cooking thread; returns true if a better plan was swapped in.
    virtual bool pollBackgroundPlan() { return false; }
    // Whether this engine may start a background planner thread. The node's Async toggle is the only
    // caller: with Async off the node must stay single-threaded (the whole point of the toggle), so
    // the deferred MEASURE/PATIENT upgrade is not started while it is off. Called once per cook
    // *before* pollBackgroundPlan(), from the pipeline owner, so it never races with the engine.
    virtual void setBackgroundAllowed(bool allowed) { (void)allowed; }
    // One line describing the live FFT library (version, path, wisdom/threads support, and whether it
    // honours the planner policy). Empty for an engine that does not use an external library. For the
    // Info DAT, so it is built on demand and never per cook.
    virtual std::string backendReport() const { return std::string(); }
};

// |X| for n_complex interleaved complex floats (raw_c and mptr 32-byte aligned).
//
// WHAT IT DOES: FFTW's real-to-complex output is n_complex pairs of (re, im) floats, and everything
// downstream (warping, weighting, dB, ballistics) works on magnitudes, so this is the one place the
// complex data is turned into a real spectrum. The interesting part is that it does the conversion
// *in the interleaved layout it arrives in*, rather than de-interleaving first.
//
// HOW: two loads of 8 floats cover 4 complex values each; _mm256_shuffle_ps pulls the real parts
// out of both loads into one register and the imaginary parts into another; one FMA forms re^2+im^2
// and the sqrt finishes it. The lane reordering described at reorderLanes below is the price of
// that shuffle trick, and it is cheaper than the alternative.
//
// WHY THE RECIPROCAL SQUARE ROOT: rsqrt + one Newton-Raphson step is accurate to ~23 bits (i.e. to
// within a float's representable precision). It is also the faster form on the target CPU: measured
// against hardware _mm256_sqrt_ps (v2.12, i9-13900H P-core, 8193 bins) rsqrt+NR is 820 ns and vsqrtps
// 1185 ns - vsqrtps ymm is ~6 cycles/instruction throughput on one port, the NR step is a few FMAs
// spread over two. Max difference between the two: 2.4e-7 relative. Keep it unless a re-measure says
// otherwise. The 1e-30 floor inside it keeps
// a zero bin from producing infinity through the reciprocal - the floor is far below any magnitude
// that survives to the dB stage, so it never changes a displayed value.
//
// The scalar tail is the whole implementation on a non-AVX2 build and uses std::sqrt there; the two
// paths agree to float precision but not bit-for-bit, which is why the vectorization tests compare
// with a tolerance rather than for equality.
inline void computeMagnitudeAVX2_FMA(const float* __restrict raw_c, float* __restrict mptr, size_t n_complex) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    // Puts the 8 magnitudes of one register back into bin order. The re/im de-interleave above uses
    // _mm256_shuffle_ps, which works within each 128-bit half, so the two loads leave the magnitudes
    // in the order 0,1,4,5,2,3,6,7 (each half of the result covers a different group of four bins).
    // _mm256_permute4x64_pd with lanes [0,2,1,3] swaps the two middle 64-bit lanes - i.e. the two
    // middle pairs of floats - which is exactly the fixup. Doing it in-register costs one permute
    // instead of splitting the store into two 128-bit halves.
    auto reorderLanes = [](__m256 mag) noexcept -> __m256 {
        return _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(mag), _MM_SHUFFLE(3, 1, 2, 0)));
    };
    // rsqrt + one Newton-Raphson step: ~23-bit accuracy at ~half the latency of sqrt_ps.
    auto fast_sqrt_ps = [](__m256 x) noexcept -> __m256 {
        __m256 xc = _mm256_max_ps(x, _mm256_set1_ps(1e-30f));
        __m256 r = _mm256_rsqrt_ps(xc);
        __m256 half = _mm256_set1_ps(0.5f);
        __m256 three = _mm256_set1_ps(3.0f);
        r = _mm256_mul_ps(_mm256_mul_ps(half, r), _mm256_fnmadd_ps(_mm256_mul_ps(xc, r), r, three));
        return _mm256_mul_ps(xc, r);
    };
    size_t n_vec16 = n_complex / 16;
    for (; i < n_vec16 * 16; i += 16) {
        __m256 cA0 = _mm256_load_ps(raw_c + 2 * i), cB0 = _mm256_load_ps(raw_c + 2 * i + 8);
        __m256 re0 = _mm256_shuffle_ps(cA0, cB0, _MM_SHUFFLE(2, 0, 2, 0));
        __m256 im0 = _mm256_shuffle_ps(cA0, cB0, _MM_SHUFFLE(3, 1, 3, 1));
        _mm256_store_ps(mptr + i, reorderLanes(fast_sqrt_ps(_mm256_fmadd_ps(re0, re0, _mm256_mul_ps(im0, im0)))));

        __m256 cA1 = _mm256_load_ps(raw_c + 2 * i + 16), cB1 = _mm256_load_ps(raw_c + 2 * i + 24);
        __m256 re1 = _mm256_shuffle_ps(cA1, cB1, _MM_SHUFFLE(2, 0, 2, 0));
        __m256 im1 = _mm256_shuffle_ps(cA1, cB1, _MM_SHUFFLE(3, 1, 3, 1));
        _mm256_store_ps(mptr + i + 8, reorderLanes(fast_sqrt_ps(_mm256_fmadd_ps(re1, re1, _mm256_mul_ps(im1, im1)))));
    }
    for (; i + 7 < n_complex; i += 8) {
        __m256 cA = _mm256_load_ps(raw_c + 2 * i), cB = _mm256_load_ps(raw_c + 2 * i + 8);
        __m256 re = _mm256_shuffle_ps(cA, cB, _MM_SHUFFLE(2, 0, 2, 0));
        __m256 im = _mm256_shuffle_ps(cA, cB, _MM_SHUFFLE(3, 1, 3, 1));
        _mm256_store_ps(mptr + i, reorderLanes(fast_sqrt_ps(_mm256_fmadd_ps(re, re, _mm256_mul_ps(im, im)))));
    }
#endif
    for (; i < n_complex; ++i) {
        float r = raw_c[2 * i], im_val = raw_c[2 * i + 1];
        mptr[i] = std::sqrt(r * r + im_val * im_val);
    }
}

/*
===========================================================================
 9. FFT ENGINE (FFTW3 ABI, backend chosen at runtime, wisdom-cached plans)
===========================================================================
The transform is computed by whichever FFTW3-ABI library the node selects - the vendored FFTW3
build or Intel oneMKL's FFTW3 interface (see FftBackend.h). Nothing here is linked against either:
the entry points are resolved once and called through the backend table, which is what lets the
choice be a parameter rather than a build decision.

Wisdom is FFTW3-specific. When the active backend supports it, it is imported once per process from
%LOCALAPPDATA%/TD_Custom_FFT/fftwf_wisdom.txt and exported after every MEASURE plan, so measured
plans cost time only the first time a size is used on this machine. A backend that reports no wisdom
support is described as such in the log instead of quietly re-planning every run.

The FFTW planner is not thread-safe: every call that creates or destroys a plan is guarded by a
process-wide mutex, which also covers a second FFT CHOP in the same project. Executing a plan on
distinct arrays is thread-safe and is not guarded.

---------------------------------------------------------------------------
HOW TO READ THIS CLASS (the IFFTEngine implementation for both FFTW3 libraries)
---------------------------------------------------------------------------
It owns exactly one thing that matters: m_plan, a transform plan for one FFT size, built by one
library. Everything else in the class exists to answer "when is it safe to build, replace, or
destroy that plan?" - which is the whole difficulty here, because the plan is created lazily, may
be upgraded by a background thread, and is destroyed by a library that may not be the one the node
is currently pointed at.

The three states a caller can observe:
  1. No plan yet (hasPlan() false) - prepare() has not run, or it failed. The pipeline turns this
     into getErrorString() rather than cooking zeros.
  2. A usable plan (hasPlan() true) - executeRFFT() runs it. This plan may be an ESTIMATE one that
     is about to be replaced: replacing it does not interrupt execution.
  3. A background measurement in flight (upgradeInProgress() true) - pollBackgroundPlan() picks up
     the result on a later cook. Nothing blocks while this runs.

The four entry points that touch the plan, and who calls them:
  prepare()             - the cooking thread, from the pipeline's rebuild path
  executeRFFT()         - the analysis worker thread, concurrently with a cook
  pollBackgroundPlan()  - the cooking thread, once per cook, before any executeRFFT
  setBackgroundAllowed()- the cooking thread, once per cook, before pollBackgroundPlan
That ordering is the engine's whole thread-safety argument: the two plan-mutating calls are
strictly serialized on the cook thread and never run while a channel is executing, so only the
background planner thread needs the mutex, and only around its own plan creation.

MEMBERS, in the order they are declared at the bottom of the class:
  m_backend / m_plan / m_fft_size / m_policy / m_planStatus / m_log  - the live plan and its owner
  m_bg_*                            - the background measurement's state and its result slot
  m_bg_allowed / m_wants_upgrade    - the Async-toggle bookkeeping (see setBackgroundAllowed)
*/
class FFTWEngine : public IFFTEngine {
public:
    FFTWEngine() = default;
    ~FFTWEngine() override {
        // Teardown is the one place that may block on a measurement: the engine (and the plugin DLL
        // with it) must not go away while a planner thread is still running inside the library.
        abandonBackground();
        reapGraveyard(true);
        if (m_plan) {
            std::lock_guard<std::mutex> lock(plannerMutex());
            if (m_backend.api.destroyPlan) m_backend.api.destroyPlan(m_plan);
            m_plan = nullptr;
        }
    }
    FFTWEngine(const FFTWEngine&) = delete;
    FFTWEngine& operator=(const FFTWEngine&) = delete;

    // The process-wide planner lock. Every plan creation and destruction takes it, because the FFTW
    // planner keeps process-global state (it is the same lock whichever engine or node asked, and
    // whichever library is in use). Executing a plan does NOT take it - that is thread-safe as long
    // as the arrays differ, which is what lets the analysis worker run while a cook plans something.
    static std::mutex& plannerMutex() { static std::mutex m; return m; }

    // Optional override of the wisdom file location (tests, portable installs). Empty = default.
    // A function-local static rather than a global so it exists exactly once across the plugin,
    // tests and bench translation units; the tests use it to point at a scratch file instead of
    // letting a test run overwrite the user's real wisdom cache.
    static std::string& wisdomPathOverride() { static std::string s; return s; }

    // Where the wisdom file lives by default: %LOCALAPPDATA%\TD_Custom_FFT\fftwf_wisdom.txt.
    // WHY LOCALAPPDATA AND NOT THE PLUGIN FOLDER: wisdom is a cache of measured plans for *this*
    // machine's CPU, and it is written by a background thread that may not have write access to the
    // installed plugin directory (a user-level Documents install does, Program Files does not).
    // LOCALAPPDATA is per-user and always writable; TEMP is the fallback if it is unset, and an
    // empty string means "no wisdom" - every caller below treats empty as "do not touch a file"
    // rather than trying to open a nameless path.
    // The directory is created here, on first call, so no separate setup step is needed.
    // The default location is resolved (and its directory created) once per process: it used to be
    // recomputed on every call, i.e. an environment lookup plus a CreateDirectoryA filesystem call each
    // time the Info DAT's fft_backend row was rendered - on the cook thread, every cook.
    static std::string wisdomPath() {
        if (!wisdomPathOverride().empty()) return wisdomPathOverride();
        static const std::string resolved = [] {
#ifdef _WIN32
            char base[MAX_PATH] = { 0 };
            DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) n = GetEnvironmentVariableA("TEMP", base, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) return std::string();
            std::string dir = std::string(base) + "\\TD_Custom_FFT";
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir + "\\fftwf_wisdom.txt";
#else
            const char* home = std::getenv("HOME");
            return home ? std::string(home) + "/.td_custom_fft_wisdom" : std::string();
#endif
        }();
        return resolved;
    }

    // Wisdom is per library, so the file has to be too. A wisdom file carries a header naming the
    // library and version that wrote it, and FFTW refuses one that does not match - so sharing a
    // single file between two backends would either be rejected on read or, on write, replace one
    // library's measured plans with the other's. The FFTW3 name is left as it was so existing
    // wisdom files keep being found; the others get the backend id appended.
    static std::string wisdomPathFor(const FftBackendInfo& info) {
        std::string base = wisdomPath();
        if (base.empty() || &info == &defaultBackend()) return base;
        const size_t dot = base.find_last_of('.');
        const std::string tag = std::string("_") + info.id;
        return (dot == std::string::npos) ? (base + tag) : (base.substr(0, dot) + tag + base.substr(dot));
    }

    // Wisdom is a property of the library, not of this node, so the import runs once per process per
    // backend even when several FFT CHOPs each build their own engine. Per backend and not once
    // globally: the two libraries keep separate planner state and separate wisdom files, so a single
    // process-wide latch would let whichever backend went first suppress the other's import. Only
    // meaningful for a backend that reports wisdom support; oneMKL accepts the calls but has nothing
    // to save, and says so through hasWisdom().
    bool importWisdomOnce(const FftApi& api, PlanLog* log) {
        static std::once_flag once[kMaxBackends];
        static bool ok[kMaxBackends] = {};
        const FftBackendInfo& info = m_backend.info ? *m_backend.info : defaultBackend();
        // Two separate reasons not to import, and only one of them is "it failed".
        if (!api.hasWisdom()) return false;
        // oneMKL exports the wisdom functions and ignores them: verified on the real mkl_rt.3.dll
        // (both symbols are in the export table) and documented by Intel, which lists the wisdom
        // save/load functions as "empty". Calling one returns 0 without creating or reading a file,
        // so importing would only produce a misleading "wisdom not found at <path>" once per
        // process - and, worse, would invite the reader to go create that file. Say what is true
        // instead, once.
        if (!info.honoursPolicy) {
            const int slot = backendIndex(info);
            std::call_once(once[slot], [this, log, &info] {
                if (log) log->log(std::string("[FFT Plugin] [") + info.logTag +
                                  "] this library has no wisdom cache: the FFTW wisdom functions are "
                                  "accepted and ignored, so there is nothing to import or export");
            });
            return false;
        }
        const int slot = backendIndex(info);
        std::call_once(once[slot], [this, &api, log, slot, &info] {
            std::string path = wisdomPathFor(info);
            if (path.empty()) return;
            {
                std::lock_guard<std::mutex> lock(plannerMutex());   // wisdom import touches planner state
                ok[slot] = api.importWisdom(path.c_str()) != 0;
            }
            if (log) log->log(std::string("[FFT Plugin] [").append(tag()) + "] wisdom " + (ok[slot] ? "loaded from " : "not found at ") + path, ok[slot]);
        });
        return ok[slot];
    }

    // Writes the current planner state to the wisdom file. Called after every successful MEASURE or
    // PATIENT plan (including the background one), so the expensive measurement is paid at most once
    // per size per machine. Not called for ESTIMATE plans - an estimate carries no measured
    // information worth saving, and writing one would put a file on disk for no benefit.
    void exportWisdom(const FftApi& api) {
        if (!api.hasWisdom()) return;
        std::string path = wisdomPathFor(m_backend.info ? *m_backend.info : defaultBackend());
        if (!path.empty()) api.exportWisdom(path.c_str());
    }

    // Tag used in every log line this engine writes, so a project with two FFT CHOPs on different
    // backends can be read apart. Short on purpose: it repeats on every plan message.
    const char* tag() const { return m_backend.info ? m_backend.info->logTag : "FFT"; }

    std::string backendReport() const override { return describeBackend(m_backend); }

    // Destroys the live plan and resets the size/status. Safe to call repeatedly and on a node that
    // never had a plan. It joins the background thread first: a measurement in flight holds a
    // separate plan that must not be left running when the engine goes away, and FFTW's planner
    // state must not be torn down underneath it.
    //
    // The destroy call goes through the *backend table*, not the library directly, because a plan
    // must be destroyed by the library that created it. That is also why the state is reset to
    // "Uninitialized" with the current tag rather than cleared to empty: getPlanStatus() must always
    // return something readable, including between a backend switch and the next prepare().
    void destroyPlan() noexcept {
        // Never wait for a background measurement here: this runs on the pipeline owner thread (the
        // cook thread when Async is off), and a FFTW_MEASURE / PATIENT run can take seconds. The
        // measurement is handed to the graveyard, finishes on its own and is reaped later.
        abandonBackground();
        reapGraveyard(false);
        if (m_plan) {
            std::lock_guard<std::mutex> lock(plannerMutex());
            if (m_backend.api.destroyPlan)
                m_backend.api.destroyPlan(m_plan);
            m_plan = nullptr;
        }
        m_fft_size = 0;
        m_planStatus = std::string(tag()) + " (Uninitialized)";
    }

    // One line for the Info DAT's plan row: backend tag plus whichever "used" string prepare() or
    // pollBackgroundPlan() last set. Never empty, so the row never reads as a blank field when there
    // is simply no plan yet.
    std::string getPlanStatus() const override {
        return m_planStatus.empty() ? (std::string(tag()) + " (Uninitialized)") : m_planStatus;
    }
    size_t fftSize() const noexcept override { return m_fft_size; }
    // A plan exists iff prepare() produced m_plan. Fast/ESTIMATE policy always does; MEASURE/Patient
    // can fail to create one (rare), in which case the pipeline must surface the failure rather than
    // silently cooking zeros.
    bool hasPlan() const noexcept override { return m_plan != nullptr; }
    // True while the background measurement is running. Read by the Info DAT to show an upgrade in
    // progress; it is an atomic load precisely because the reader is the cook thread and the writer
    // is the planner thread.
    bool upgradeInProgress() const noexcept { return m_bg && !m_bg->done.load(std::memory_order_acquire); }
    // Measurements abandoned by a size/backend change that are still running (telemetry).
    size_t abandonedMeasurements() const noexcept { return m_graveyard.size(); }

    /*
      Planner policies
        Fast     : FFTW_ESTIMATE only (never stalls, generic plan).
        Measured : FFTW_MEASURE synchronously (best plan; ~0.4 s once per size per machine, then cached in wisdom).
        Auto     : if wisdom already holds a measured plan for this size (FFTW_MEASURE | FFTW_WISDOM_ONLY) use it
                   instantly; otherwise use an ESTIMATE plan right away and measure a better one on a background
                   thread, swap it in on the next cook (pollBackgroundPlan) and save it to wisdom. Real-time is never
                   interrupted and the second run of any size is already optimal.
        Patient  : same scheme with FFTW_PATIENT, no time limit (measured: 10-15 % faster execute than MEASURE at
                   16K/32K; ~2.7 s of planning at N = 32768 on an i9-13900H, longer on slower machines, once per
                   size per machine). The cook thread is never blocked in the steady state - planning happens on
                   the background thread, after the node already has an ESTIMATE plan in hand.
                   Patient wisdom also satisfies Auto's lookup, so once a size has been planned patiently every
                   policy but Fast benefits.
                   Caveat: FFTW's planner is process-wide and single-threaded, so a plan request (a size
                   change, another instance) that arrives while a patient measurement is running waits on
                   the planner lock until that measurement ends. What waits is whoever asked next - the
                   patient node itself already has its ESTIMATE plan and keeps cooking through it; with Async
                   on the waiter is a worker thread, not TouchDesigner's cook.
    */
    void prepare(size_t fft_size, PlannerPolicy policy, PlanLog* log,
                 const FftBackendInfo* backend = nullptr) override {
        const FftBackendInfo* want =
            backend ? backend : (m_backend.info ? m_backend.info : &defaultBackend());

        // Re-planning is not free even when nothing looks changed: it destroys the plan under the
        // process-wide planner lock, and a synchronous policy then re-measures and re-exports wisdom.
        // So the early-out compares the policy and the backend too, and returns before any of that.
        if (m_fft_size == fft_size && m_plan != nullptr && m_policy == policy && m_backend.info == want)
            return;

        // Destroy the outgoing plan with the outgoing library, before switching. A plan belongs to
        // the library that created it; passing it to the other library's destroy_plan is undefined.
        destroyPlan();
        if (fft_size == 0) return;

        // Selecting a backend costs nothing after the first time: the library is loaded once per
        // process and cached, so this only re-points at it. Toggling back and forth re-plans, which
        // is the real cost of a switch, and is why the early-out above compares m_backend.info.
        if (m_backend.info != want) {
            const FftBackendInfo* previous = m_backend.info;
            m_backend = cachedBackend(*want);
            if (log && previous != m_backend.info) {
                log->log(std::string("[FFT Plugin] [") + want->logTag + "] backend: " +
                             (m_backend.loaded() ? (m_backend.name() + std::string(" from ") + m_backend.path)
                                                 : m_backend.error),
                         m_backend.loaded());
            }
        }
        // A selected library that is not installed must not leave the node without a plan - the node
        // would then publish a flat spectrum and look broken rather than misconfigured. Fall back to
        // the vendored FFTW3 build, which ships beside the plugin, and say so at error level: the
        // user asked for something this machine does not have.
        if (!m_backend.loaded() && want != &defaultBackend()) {
            if (log) {
                log->log(std::string("[FFT Plugin] [") + want->logTag + "] ERROR: " + m_backend.error);
                log->log(std::string("[FFT Plugin] [") + want->logTag +
                         "] falling back to " + defaultBackend().display +
                         " for this node; install the selected library next to FFT.dll to use it");
            }
            want = &defaultBackend();
            m_backend = cachedBackend(*want);
        }
        if (!m_backend.loaded()) {
            m_planStatus = std::string(want->logTag) + " (no library)";
            if (log) log->log(std::string("[FFT Plugin] [") + want->logTag + "] ERROR: " + m_backend.error);
            return;
        }
        const FftApi& api = m_backend.api;
        m_fft_size = fft_size;
        m_policy = policy;
        m_log = log;

        importWisdomOnce(api, log);

        auto t0 = std::chrono::high_resolution_clock::now();
        const char* used = "FFTW_ESTIMATE";
        bool start_background = false;
        unsigned bg_rigor = FFTW_MEASURE;
        // Reset with the plan it describes: a plan built from wisdom needs no upgrade, and leaving a
        // stale true here would let setBackgroundAllowed() re-arm a measurement for a plan that is
        // already the measured one.
        m_wants_upgrade = false;
        {
            std::lock_guard<std::mutex> lock(plannerMutex());
            Buffers b(api, fft_size);
            if (!b.ok()) { m_planStatus = std::string(tag()) + " (allocation failed)"; return; }
            if (!want->honoursPolicy) {
                // The library plans by its own rules and ignores FFTW's rigour flags, so there is no
                // wisdom lookup, no background measurement, and no claim in the status that a policy
                // was applied. The flag is still passed because it is part of the call signature.
                m_plan = api.planR2C(static_cast<int>(fft_size), b.in, b.out, FFTW_ESTIMATE);
                used = "library default plan (FFT planner policy does not apply)";
            } else switch (policy) {
                case PlannerPolicy::Measured:
                    m_plan = api.planR2C(static_cast<int>(fft_size), b.in, b.out, FFTW_MEASURE);
                    used = "FFTW_MEASURE";
                    if (m_plan) exportWisdom(api);
                    break;
                case PlannerPolicy::Fast:
                    m_plan = api.planR2C(static_cast<int>(fft_size), b.in, b.out, FFTW_ESTIMATE);
                    break;
                case PlannerPolicy::Patient:
                    m_plan = api.planR2C(static_cast<int>(fft_size), b.in, b.out, FFTW_PATIENT | FFTW_WISDOM_ONLY);
                    if (m_plan) {
                        used = "FFTW_PATIENT (from wisdom)";
                    } else {
                        m_plan = api.planR2C(static_cast<int>(fft_size), b.in, b.out, FFTW_ESTIMATE);
                        used = "FFTW_ESTIMATE (patient measure in background)";
                        start_background = true;
                        bg_rigor = FFTW_PATIENT;
                    }
                    break;
                case PlannerPolicy::Auto:
                default:
                    m_plan = api.planR2C(static_cast<int>(fft_size), b.in, b.out, FFTW_MEASURE | FFTW_WISDOM_ONLY);
                    if (m_plan) {
                        used = "FFTW_MEASURE (from wisdom)";
                    } else {
                        m_plan = api.planR2C(static_cast<int>(fft_size), b.in, b.out, FFTW_ESTIMATE);
                        used = "FFTW_ESTIMATE (measuring in background)";
                        start_background = true;
                    }
                    break;
            }
            if (!m_plan && want->honoursPolicy && policy != PlannerPolicy::Fast) {
                m_plan = api.planR2C(static_cast<int>(fft_size), b.in, b.out, FFTW_ESTIMATE);
                used = "FFTW_ESTIMATE (fallback)";
            }
        }
        // Say what actually happened, not what the policy would have liked: with Async off there is no
        // background thread to measure on, so the status must not keep claiming "measuring in
        // background". That string is what the Info DAT and the plan row show, and a user reading it
        // would wait for an upgrade that is never coming.
        const bool defer_upgrade = start_background && !m_bg_allowed;
        if (defer_upgrade) used = "FFTW_ESTIMATE (measured upgrade deferred: Async is off)";
        double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        if (m_plan) {
            m_planStatus = std::string(tag()) + " (" + used + " - " + std::to_string(ms) + " ms, N=" + std::to_string(fft_size) + ")";
            if (log) {
                log->log(std::string("[FFT Plugin] [") + tag() + "] plan N=" + std::to_string(fft_size) + " " + used + " in " + std::to_string(ms) + " ms");
                // Which library is doing the work, and which SIMD kernels it chose for this plan: the
                // only way to tell an AVX2 build from an SSE2 one, or FFTW from oneMKL, at runtime -
                // they produce identical results and accept identical calls. Printed in full when the
                // backend description changes (first plan, a backend switch); on a size change of the
                // same backend only a changed kernel note is printed, so resizing does not repeat the
                // same long line for every plan (v2.12).
                const std::string desc = describeBackend(m_backend);
                const std::string simd = describePlanSimd(api, m_plan);
                if (desc != m_loggedBackend) {
                    log->log(std::string("[FFT Plugin] [") + tag() + "] " + desc + " - " + simd, backendVersionMatches(m_backend));
                    m_loggedBackend = desc;
                    m_loggedSimd = simd;
                } else if (simd != m_loggedSimd) {
                    log->log(std::string("[FFT Plugin] [") + tag() + "] " + simd);
                    m_loggedSimd = simd;
                }
            }
        } else {
            m_planStatus = std::string(tag()) + " (plan creation FAILED)";
            if (log) log->log(std::string("[FFT Plugin] [") + tag() + "] ERROR: plan creation failed for N=" + std::to_string(fft_size));
            return;
        }
        if (start_background) {
            m_wants_upgrade = true;
            if (!defer_upgrade) {
                startBackgroundMeasure(fft_size, bg_rigor);
            } else if (log) {
                // Async is off, so the node is deliberately single-threaded: the measured upgrade is
                // deferred rather than run on the cook thread, where the same measurement costs
                // 0.7-2.7 s at the sizes this plugin uses (measured: N=65536 FFTW_MEASURE 805 ms,
                // N=2048 FFTW_PATIENT 675 ms) and would stall the whole TouchDesigner frame. The
                // plan in use stays a correct ESTIMATE one; it is only a few percent slower to
                // execute, and the node keeps cooking at full rate throughout. Two ways to get the
                // upgrade: turn Async on (it then measures off the cook thread and swaps itself in),
                // or set Planner Policy = Measured, which takes the stall deliberately, once.
                log->log(std::string("[FFT Plugin] [") + tag() + "] Async is off, so the " +
                             ((bg_rigor == FFTW_PATIENT) ? "FFTW_PATIENT" : "FFTW_MEASURE") +
                             " upgrade for N=" + std::to_string(fft_size) +
                             " is deferred (planning stays on the cook thread, nothing runs off it). "
                             "Turn Async on to measure in the background, or set Planner Policy = "
                             "Measured to measure once synchronously");
            }
        }
    }

    // Transforms one already-windowed block and writes its magnitudes. Called once per channel per
    // cook, on the analysis worker thread, while the cook thread may be doing anything else.
    //
    // WHAT IT GUARANTEES ABOUT ITS OUTPUT SIZE: magnitude_spectrum is always sized n/2+1 (the full
    // real-to-complex bin count) so the caller's buffers never change size mid-run - a resize here
    // would be an allocation on the real-time path. n_mag then limits how much of it is actually
    // computed; see the round-up note below.
    //
    // THE ZERO-FILL BRANCH is the "there is no plan" case, and it is deliberate rather than an
    // error return: this function is noexcept and runs off the cook thread, so it cannot throw or
    // log. Writing zeros keeps every downstream stage working on a well-formed array (the pipeline
    // reports the missing plan through hasPlan() separately), which is far easier to reason about
    // than a half-written buffer.
    void executeRFFT(const AlignedVector& padded_signal, AlignedVector& magnitude_spectrum,
                     AlignedComplexVector& scratch_complex, size_t n_mag = 0) const noexcept override {
        size_t n = padded_signal.size();
        size_t n_complex = n / 2 + 1;
        if (magnitude_spectrum.size() != n_complex) magnitude_spectrum.resize(n_complex);
        if (scratch_complex.size() != n_complex) scratch_complex.resize(n_complex);
        // The size check against m_fft_size is not redundant with hasPlan(): a plan built for a
        // different N would be executed on the wrong-length buffer, and FFTW would read past it.
        // This can only happen in the window between a parameter change and the rebuild, and the
        // zero-fill above is the correct answer for that one frame.
        if (m_plan && n == m_fft_size && m_backend.api.executeR2C) {
            // const_cast: FFTW's r2c execution signature takes a non-const input pointer, but with
            // FFTW_ESTIMATE/MEASURE plans and no FFTW_PRESERVE_INPUT flag the input array is only
            // read. This is the documented FFTW contract, not a shortcut around it.
            float* in_ptr = const_cast<float*>(padded_signal.data());
            fftwf_complex* out_ptr = reinterpret_cast<fftwf_complex*>(scratch_complex.data());
            m_backend.api.executeR2C(m_plan, in_ptr, out_ptr);
        } else {
            std::memset(scratch_complex.data(), 0, n_complex * sizeof(std::complex<float>));
        }
        // Round the request up to a whole 16-bin SIMD block: the vector loop below writes 16 at a
        // time, and a partial final block is not worth a scalar epilogue. So the caller may find up
        // to 15 bins past n_mag written - it only ever reads up to maxLinearIndex() anyway.
        size_t count = (n_mag == 0) ? n_complex : std::min(n_complex, ((n_mag + 15) / 16) * 16);
        computeMagnitudeAVX2_FMA(reinterpret_cast<const float*>(scratch_complex.data()), magnitude_spectrum.data(), std::min(count, n_complex));
    }

private:
    // Scratch for plan creation, allocated and freed by the active library's own allocator. FFTW
    // plans are built against buffers whose alignment and ownership the planner is told about, and
    // oneMKL allocates through its own runtime; the api is held so the destructor returns the memory
    // to the same library that handed it out. That is why this is not a wrapper around new[].
    struct Buffers {
        const FftApi& api;
        float* in{ nullptr };
        fftwf_complex* out{ nullptr };
        Buffers(const FftApi& a, size_t n) : api(a) {
            in  = static_cast<float*>(api.alloc(sizeof(float) * n));
            out = static_cast<fftwf_complex*>(api.alloc(sizeof(fftwf_complex) * (n / 2 + 1)));
        }
        Buffers(const Buffers&) = delete;
        Buffers& operator=(const Buffers&) = delete;
        ~Buffers() {
            if (in)  api.dealloc(in);
            if (out) api.dealloc(out);
        }
        bool ok() const { return in && out; }
    };

    // One background measurement. Owned by the engine while it is the live upgrade (m_bg), then by the
    // graveyard if a size or backend change abandons it before it finishes. The thread touches only this
    // struct and the library table copied into it - never the engine - so an abandoned measurement can
    // outlive the plan it was meant to upgrade without racing anything.
    struct BgTask {
        std::thread thread;
        std::atomic<fftwf_plan> plan{ nullptr };
        std::atomic<bool> done{ false };
        FftApi api;
        size_t size{ 0 };
        unsigned rigor{ FFTW_MEASURE };
        double ms{ 0.0 };
    };

    // No time limit on a FFTW_PATIENT measurement (v2.12.1; v2.10-v2.12 capped it at 1.5 s with
    // fftwf_set_timelimit). The measurement runs on this background thread and never on the cook
    // thread, so a slower machine may take as long as it needs to find the best plan: ~2.7 s at
    // N = 32768 on the i9-13900H, longer elsewhere, once per size per machine (wisdom caches it).
    // What an unbounded measurement can still make WAIT, because FFTW's planner is process-wide and
    // serialised by plannerMutex():
    //   * a re-plan of this or another node (prepare() takes the lock blocking). With Async on that is
    //     the analysis worker, so TouchDesigner keeps cooking and the node holds its last spectrum;
    //     with Async off it is the cook thread (no new measurement starts while Async is off, but one
    //     already running when it was switched off finishes first);
    //   * teardown (~FFTWEngine joins the planner thread before the library can go away).

    // rigor: FFTW_MEASURE or FFTW_PATIENT. Runs at ABOVE_NORMAL: it must never be starved below the
    // normal-priority threads it is racing, so that a plan upgrade finishes in its budget instead of
    // stretching out under load. It is still one notch under the analysis worker (HIGHEST).
    void startBackgroundMeasure(size_t fft_size, unsigned rigor) {
        abandonBackground();
        reapGraveyard(false);
        auto task = std::make_unique<BgTask>();
        task->api = m_backend.api;           // copied: the owner may re-point m_backend meanwhile
        task->size = fft_size;
        task->rigor = rigor;
        const std::string wisdom = task->api.hasWisdom()
            ? wisdomPathFor(m_backend.info ? *m_backend.info : defaultBackend()) : std::string();
        BgTask* t = task.get();
        task->thread = std::thread([t, wisdom]() {
#ifdef _WIN32
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
            if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
                if (auto fn = reinterpret_cast<SetDescFn>(GetProcAddress(k32, "SetThreadDescription")))
                    fn(GetCurrentThread(), L"FFT background planner");
            }
#endif
            auto t0 = std::chrono::high_resolution_clock::now();
            fftwf_plan p = nullptr;
            {
                std::lock_guard<std::mutex> lock(plannerMutex());   // planner is single-threaded; execute() is thread-safe
                Buffers b(t->api, t->size);
                if (b.ok()) p = t->api.planR2C(static_cast<int>(t->size), b.in, b.out, t->rigor);
                if (p && !wisdom.empty()) t->api.exportWisdom(wisdom.c_str());
            }
            t->ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
            t->plan.store(p, std::memory_order_release);
            t->done.store(true, std::memory_order_release);
        });
        m_bg = std::move(task);
        m_bg_size = fft_size;
        m_bg_rigor = rigor;
    }

public:
    // Call once per cook from the cooking thread (before any channel executes). Swaps in a
    // background-measured plan when one is ready. Returns true when the plan changed.
    bool pollBackgroundPlan() override {
        reapGraveyard(false);
        if (!m_bg || !m_bg->done.load(std::memory_order_acquire)) return false;
        fftwf_plan ready = m_bg->plan.exchange(nullptr);
        if (!ready) {                                  // the measurement produced no plan: nothing to swap
            if (m_bg->thread.joinable()) m_bg->thread.join();   // done -> returns immediately
            m_bg.reset();
            return false;
        }
        // try_lock, not lock: another node's measurement may hold the process-wide planner lock for as
        // long as it takes (PATIENT has no time limit). The ready plan waits for the next job instead of
        // stalling this thread.
        std::unique_lock<std::mutex> lock(plannerMutex(), std::try_to_lock);
        if (!lock.owns_lock()) { m_bg->plan.store(ready); return false; }
        if (m_bg->size != m_fft_size || m_bg->api.destroyPlan != m_backend.api.destroyPlan) {   // size/library changed: discard
            if (m_bg->api.destroyPlan) m_bg->api.destroyPlan(ready);
            lock.unlock();
            if (m_bg->thread.joinable()) m_bg->thread.join();
            m_bg.reset();
            return false;
        }
        if (m_plan && m_backend.api.destroyPlan) m_backend.api.destroyPlan(m_plan);
        m_plan = ready;
        lock.unlock();
        if (m_bg->thread.joinable()) m_bg->thread.join();
        const double ms = m_bg->ms;
        m_bg.reset();
        m_wants_upgrade = false;
        const char* rigor = (m_bg_rigor == FFTW_PATIENT) ? "FFTW_PATIENT" : "FFTW_MEASURE";
        m_planStatus = std::string(tag()) + " (" + rigor + " upgraded in background - " + std::to_string(ms) + " ms, N=" + std::to_string(m_fft_size) + ")";
        if (m_log) m_log->log(std::string("[FFT Plugin] [") + tag() + "] background " + rigor + " plan ready for N=" + std::to_string(m_fft_size) + " (" + std::to_string(ms) + " ms), swapped in");
        return true;
    }

    // The node's Async toggle, delivered per cook. Two behaviours, and the reason each is safe:
    //  * true  -> allowed again. If the live plan is an ESTIMATE that was only left un-upgraded
    //              because background work was disallowed (m_wants_upgrade), the measurement starts
    //              now. Without this, turning Async back on would leave the node on ESTIMATE forever:
    //              prepare()'s early-out sees an unchanged size/policy/backend and never re-plans.
    //  * false -> disallowed. Nothing is stopped or joined: a measurement already running finishes
    //              and is swapped in by pollBackgroundPlan() (one-shot, and the result is a better
    //              plan for the same library - discarding it would be pure waste). What is prevented
    //              is *starting* one, and the deferred log in prepare() only fires when setBackgroundAllowed(false)
    //              was seen before the plan was made.
    void setBackgroundAllowed(bool allowed) override {
        if (allowed == m_bg_allowed) return;
        m_bg_allowed = allowed;
        if (allowed && m_wants_upgrade && m_plan && !upgradeInProgress() && m_fft_size != 0)
            startBackgroundMeasure(m_fft_size, m_bg_rigor);
    }

private:
    // Hands the live measurement (if any) to the graveyard without waiting for it. A measurement that
    // already finished is disposed of right here when the planner lock is free.
    void abandonBackground() noexcept {
        if (!m_bg) return;
        m_graveyard.push_back(std::move(m_bg));
        m_bg.reset();
    }

    // Joins and disposes of abandoned measurements. block = false (the owner thread, every job): only
    // tasks that are already done, and only if the planner lock is free right now - cost when there is
    // nothing to do is one empty() check. block = true (teardown): waits for every one of them.
    void reapGraveyard(bool block) noexcept {
        if (m_graveyard.empty()) return;
        for (size_t i = 0; i < m_graveyard.size();) {
            BgTask& t = *m_graveyard[i];
            if (!block && !t.done.load(std::memory_order_acquire)) { ++i; continue; }
            try { if (t.thread.joinable()) t.thread.join(); } catch (...) {}
            if (fftwf_plan leftover = t.plan.exchange(nullptr)) {
                std::unique_lock<std::mutex> lock(plannerMutex(), std::defer_lock);
                if (block) lock.lock();
                else if (!lock.try_lock()) { t.plan.store(leftover); ++i; continue; }   // retry next time
                if (t.api.destroyPlan) t.api.destroyPlan(leftover);
            }
            m_graveyard.erase(m_graveyard.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // ---- the live plan ----------------------------------------------------
    FftBackend m_backend;                  // active library; loaded once per process and cached
    fftwf_plan m_plan{ nullptr };          // owned by m_backend's library; see destroyPlan()
    size_t m_fft_size{ 0 };                // N the plan was built for; 0 = no plan
    PlannerPolicy m_policy{ PlannerPolicy::Auto };  // the policy m_plan was built under (part of the
                                           // prepare() early-out key, so changing policy re-plans)
    std::string m_planStatus;              // empty = not prepared yet; getPlanStatus() names the tag
    std::string m_loggedBackend, m_loggedSimd;   // the backend line / kernel note last printed (prepare)
    PlanLog* m_log{ nullptr };             // not owned; the node's log, used for plan messages

    // ---- the background measurement ---------------------------------------
    // The live measurement (m_bg) and the ones a size/backend change abandoned (m_graveyard). The plan
    // comes back through BgTask's atomic pointer, so polling it never waits on the planner thread.
    std::unique_ptr<BgTask> m_bg;
    std::vector<std::unique_ptr<BgTask>> m_graveyard;
    size_t m_bg_size{ 0 };                 // N the background thread was asked to plan for
    unsigned m_bg_rigor{ FFTW_MEASURE };   // FFTW_MEASURE or FFTW_PATIENT - which upgrade is wanted,
                                           // carried from prepare() through to the status string
    // The node's Async state, as last delivered by setBackgroundAllowed(). Starts true: the engine may
    // be driven by a caller (the headless tests and the bench) that never says otherwise, and those
    // want the background upgrade. See setBackgroundAllowed() for the full contract.
    bool m_bg_allowed{ true };
    // The live plan is an ESTIMATE that a rigour policy wanted upgraded, but the measurement has not
    // been taken (yet). Set in prepare(), and the trigger for the re-arm in setBackgroundAllowed().
    bool m_wants_upgrade{ false };
};

} // namespace FFTDSP

#endif // DSP_MODULES_H

/*
End of DSPModules.h. Everything above is header-only (no .cpp exists for this file): the plugin,
the test suite and the bench each compile it into their own translation unit, which is why every
function here is inline or a class member, and why nothing in it may reference a TouchDesigner type.
If you are looking for the code that drives this pipeline per cook, that is FFT.cpp;
for the per-channel order of stages, AnalysisPipeline.cpp; for the parameters, Parameters.h/.cpp.
*/
