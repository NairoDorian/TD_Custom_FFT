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
8. Dynamic Range Conversion: dB with selectable reference, interpolated LUT log10 (AVX2).
9. Temporal Smoothing: Asymmetric attack/release envelope (2x unrolled FMA).

SIMD alignment policy
---------------------
AVX2 code uses ALIGNED loads/stores only where alignment is guaranteed by construction
(buffer base pointers from AlignedAllocator, offsets that are multiples of 8 floats).
Everywhere else the unaligned variants are used. MSVC emits vmovups for both, so this
is a correctness/portability guarantee (Clang/GCC would fault on a misaligned vmovaps),
not a performance change.
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

#include <fftw3.h>     // FFTW3 Fast Fourier Transform Library (Single Precision: fftwf_*)

/*
===========================================================================
  0. 32-BYTE ALIGNED ALLOCATOR (for AVX2 SIMD load/store alignment)
===========================================================================
*/
template<typename T, std::size_t Align = 32>
struct AlignedAllocator {
    using value_type = T;

    template<typename U> struct rebind { using other = AlignedAllocator<U, Align>; };

    AlignedAllocator() noexcept = default;
    template<typename U> AlignedAllocator(const AlignedAllocator<U, Align>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 0) return nullptr;
#ifdef _MSC_VER
        void* ptr = _aligned_malloc(n * sizeof(T), Align);
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, Align, n * sizeof(T)) != 0) ptr = nullptr;
#endif
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
    bool operator==(const AlignedAllocator&) const noexcept { return true; }
    bool operator!=(const AlignedAllocator&) const noexcept { return false; }
};

namespace FFTDSP {

using AlignedVector = std::vector<float, AlignedAllocator<float, 32>>;
using AlignedComplexVector = std::vector<std::complex<float>, AlignedAllocator<std::complex<float>, 32>>;

constexpr float PI_F = 3.14159265358979323846f;
constexpr double PI_D = 3.14159265358979323846;

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
*/
struct DenormalGuard {
    unsigned int saved;
    DenormalGuard() noexcept : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }   // FTZ (bit 15) | DAZ (bit 6)
    ~DenormalGuard() noexcept { _mm_setcsr(saved); }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;
};

// True if every sample is exactly 0.0 / -0.0 (digital silence). ~0.05 us for 735 samples.
inline bool blockIsSilent(const float* x, size_t n) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    __m256i acc = _mm256_setzero_si256();
    const __m256i mask = _mm256_set1_epi32(0x7FFFFFFF);
    for (; i + 7 < n; i += 8) {
        acc = _mm256_or_si256(acc, _mm256_and_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(x + i)), mask));
    }
    if (!_mm256_testz_si256(acc, acc)) return false;
#endif
    for (; i < n; ++i) {
        uint32_t bits; std::memcpy(&bits, x + i, 4);
        if (bits & 0x7FFFFFFFu) return false;
    }
    return true;
}

// dst[i] = (a[i] + b[i]) (unaligned ok)
inline void addInto(const float* __restrict a, const float* __restrict b, float* __restrict dst, size_t n) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 7 < n; i += 8) _mm256_storeu_ps(dst + i, _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
#endif
    for (; i < n; ++i) dst[i] = a[i] + b[i];
}

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
*/
inline bool cpuSupportsAVX2() noexcept {
#if defined(_WIN32)
    // PF_AVX2_INSTRUCTIONS_AVAILABLE == 40 (Windows 10+)
    if (IsProcessorFeaturePresent(40)) return true;
    int info[4] = { 0, 0, 0, 0 };
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuidex(info, 7, 0);
    const bool avx2 = (info[1] & (1 << 5)) != 0;
    __cpuid(info, 1);
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool fma = (info[2] & (1 << 12)) != 0;
    if (!avx2 || !osxsave || !fma) return false;
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
*/
constexpr size_t kMaxPlanLogEntries = 256;

#ifdef _WIN32
namespace python_logger {

enum class GILState { Locked, Unlocked };
using EnsureFn  = GILState(*)(void);
using ReleaseFn = void(*)(GILState);
using WriteFn   = void(*)(const char*, ...);

struct Api {
    EnsureFn  ensure = nullptr;
    ReleaseFn release = nullptr;
    WriteFn   write = nullptr;
    bool      ok = false;
};

inline const Api& api() {
    static const Api resolved = [] {
        Api a;
        const char* names[] = { "python311.dll", "python312.dll", "python313.dll", "python310.dll", "python3.dll" };
        HMODULE h = nullptr;
        for (const char* n : names) { h = GetModuleHandleA(n); if (h) break; }
        if (!h) return a;
        a.ensure  = reinterpret_cast<EnsureFn>(GetProcAddress(h, "PyGILState_Ensure"));
        a.release = reinterpret_cast<ReleaseFn>(GetProcAddress(h, "PyGILState_Release"));
        a.write   = reinterpret_cast<WriteFn>(GetProcAddress(h, "PySys_WriteStdout"));
        a.ok = a.ensure && a.release && a.write;
        return a;
    }();
    return resolved;
}

inline void writeToTextport(const std::string& msg) {
    const Api& a = api();
    if (!a.ok) return;
    GILState g = a.ensure();
    // PySys_WriteStdout truncates at 1000 bytes; split long messages.
    std::string line = msg;
    for (char& c : line) if (c == '\r') c = ' ';
    size_t pos = 0;
    while (pos < line.size()) {
        std::string chunk = line.substr(pos, 900);
        a.write("%s%s", chunk.c_str(), (pos + 900 >= line.size()) ? "\n" : "");
        pos += 900;
    }
    a.release(g);
}

} // namespace python_logger
#endif

class PlanLog {
public:
    // deferred = true: messages are queued and written to the Textport by flushToTextport()
    // (call it from the cooking thread). Worker threads must never call into Python directly.
    void setDeferred(bool deferred) { std::lock_guard<std::mutex> lock(m_mutex); m_deferred = deferred; }

    void log(const std::string& msg, bool echoToTextport = true) {
        bool deferred;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_history.size() >= kMaxPlanLogEntries) {
                m_history.erase(m_history.begin(), m_history.begin() + static_cast<std::ptrdiff_t>(m_history.size() / 2));
            }
            m_history.push_back(msg);
            deferred = m_deferred;
            if (echoToTextport && deferred) m_pending.push_back(msg);
        }
#ifdef _WIN32
        if (echoToTextport && !deferred) python_logger::writeToTextport(msg);
#else
        (void)echoToTextport;
#endif
    }

    // Write queued messages to the Textport (cooking thread only). Returns the number written.
    size_t flushToTextport() {
        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            pending.swap(m_pending);
        }
#ifdef _WIN32
        for (const auto& m : pending) python_logger::writeToTextport(m);
#endif
        return pending.size();
    }
    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_history;
    }
    // Single entry (used per Info DAT row so the whole history is not copied for every row)
    std::string entry(size_t i) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return i < m_history.size() ? m_history[i] : std::string();
    }
    size_t size() const { std::lock_guard<std::mutex> lock(m_mutex); return m_history.size(); }
    void clear() { std::lock_guard<std::mutex> lock(m_mutex); m_history.clear(); }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_history;
    std::deque<std::string> m_pending;
    bool m_deferred{ false };
};

/*
===========================================================================
 1. CIRCULAR RING BUFFER (FIFOBuffer)
===========================================================================
*/
class FIFOBuffer {
public:
    explicit FIFOBuffer(size_t capacity = 3175) { resize(capacity); }

    void resize(size_t capacity) {
        m_capacity = std::max<size_t>(1, capacity);
        m_data.assign(m_capacity, 0.0f);
        m_idx = 0;
        m_filled = 0;
    }

    size_t capacity() const noexcept { return m_capacity; }
    size_t filled() const noexcept { return m_filled; }

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
            std::memcpy(m_data.data() + m_idx, signal, count * sizeof(float));
            m_idx = end % m_capacity;
        } else {
            size_t first = m_capacity - m_idx;
            std::memcpy(m_data.data() + m_idx, signal, first * sizeof(float));
            std::memcpy(m_data.data(), signal + first, (count - first) * sizeof(float));
            m_idx = count - first;
        }
        if (m_filled < m_capacity) m_filled = std::min(m_capacity, m_filled + count);
    }

    // Linearized copy of the buffer (oldest sample first); right-aligned & zero-padded while filling.
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
*/
struct BiquadSection {
    float b0{ 1.0f }, b1{ 0.0f }, b2{ 0.0f };
    float a1{ 0.0f }, a2{ 0.0f };
    float z1{ 0.0f }, z2{ 0.0f };
    bool active{ false };

    inline float process(float x) noexcept {
        if (!active) return x;
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = 0.0f; z2 = 0.0f; }
};

class BiquadEQ {
public:
    explicit BiquadEQ(double sampling_rate = 44100.0) : m_sample_rate(sampling_rate) {}

    void setSampleRate(double sr) noexcept {
        if (m_sample_rate != sr) {
            m_sample_rate = sr;
            m_design_key = std::make_tuple(0.0, 0.0, 0.0, 0.0, 0.0);
        }
    }

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

    void reset() noexcept { m_high_shelf.reset(); m_low_shelf.reset(); }

private:
    double m_sample_rate{ 44100.0 };
    BiquadSection m_high_shelf;
    BiquadSection m_low_shelf;
    std::tuple<double, double, double, double, double> m_design_key;
};

/*
===========================================================================
 3. WINDOW GENERATOR
===========================================================================
Normalization modes:
  CoherentGain : mean(window) == 1  (legacy behaviour; peak of a full-scale sine == N_win/2)
  FullScale    : sum(window) == 2   (one-sided spectrum of a sine with amplitude A reads A)
*/
enum class WindowNorm : int { CoherentGain = 0, FullScale = 1 };

class WindowGenerator {
public:
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

    // window_type: 0 Kaiser, 1 Hann, 2 Hamming, 3 Blackman, 4 Blackman-Harris, 5 Rectangular
    static void generateWindow(int window_type, double kaiser_beta, size_t length, AlignedVector& window,
                               WindowNorm norm = WindowNorm::CoherentGain) {
        window.resize(length);
        if (length == 0) return;
        double denom = (length > 1) ? static_cast<double>(length - 1) : 1.0;
        double sum = 0.0;
        double kaiser_inv_I0 = 1.0 / besselI0(kaiser_beta);

        for (size_t n = 0; n < length; ++n) {
            double w = 1.0;
            double fn = static_cast<double>(n);
            switch (window_type) {
                case 1: w = 0.5 - 0.5 * std::cos(2.0 * PI_D * fn / denom); break;
                case 2: w = 0.54 - 0.46 * std::cos(2.0 * PI_D * fn / denom); break;
                case 3: w = 0.42 - 0.5 * std::cos(2.0 * PI_D * fn / denom) + 0.08 * std::cos(4.0 * PI_D * fn / denom); break;
                case 4: w = 0.35875 - 0.48829 * std::cos(2.0 * PI_D * fn / denom)
                            + 0.14128 * std::cos(4.0 * PI_D * fn / denom)
                            - 0.01168 * std::cos(6.0 * PI_D * fn / denom); break;
                case 5: w = 1.0; break;
                case 0:
                default: {
                    double term = 2.0 * fn / denom - 1.0;
                    double arg = std::sqrt(std::max(0.0, 1.0 - term * term));
                    w = besselI0(kaiser_beta * arg) * kaiser_inv_I0;
                    break;
                }
            }
            window[n] = static_cast<float>(w);
            sum += w;
        }
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
Tables: uint32 i0 (i1 = i0 + 1 implicit) + float weight = 8 bytes per output bin
(was 20 bytes with size_t i0/i1). AVX2 path gathers src[i0] and src[i0+1]
with a single index vector and blends with one FMA.
*/
class PerceptualWarping {
public:
    static double htkHzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
    static double htkMelToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

    static double erbRateGlasberg(double hz) { double x = hz / 123.0; return 6.230 * (x * x) + 93.390 * x + 28.520; }
    static double erbRateToHz(double erb) {
        double a = 6.230, b = 93.390, c = 28.520;
        double x = (-b + std::sqrt(std::max(0.0, b * b - 4.0 * a * (c - erb)))) / (2.0 * a);
        return x * 123.0;
    }

    static double hzToBark(double hz) {
        double z = (26.81 * hz) / (1960.0 + hz) - 0.53;
        if (z < 2.0) z += 0.15 * (2.0 - z);
        else if (z > 20.1) z += 0.22 * (z - 20.1);
        return z;
    }
    static double barkToHz(double bark) {
        double z = bark;
        if (z < 2.0) z = (z - 0.3) / 0.85;
        else if (z > 20.1) z = (z - 4.422) / 0.78;
        double f = (1960.0 * (z + 0.53)) / (26.81 - (z + 0.53));
        return std::max(0.0, f);
    }

    static double hzToChroma(double hz) { double f = std::max(1e-5, hz); return 12.0 * std::log2(f / 440.0) + 69.0; }
    static double chromaToHz(double chroma) { return 440.0 * std::pow(2.0, (chroma - 69.0) / 12.0); }

    // scale_code: 0 Log, 1 Mel, 2 ERB, 3 Bark, 4 Chroma, 5 Linear, 6 Mel+Log blend
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

    // 0 = linear (2 taps), 1 = Catmull-Rom cubic (4 taps, smoother lobes -> allows a smaller FFT)
    void setInterpolation(int mode) noexcept { m_interp = (mode == 1) ? 1 : 0; }
    int interpolation() const noexcept { return m_interp; }

    // Highest linear bin index the warp reads (+ cubic look-ahead). Magnitudes above it need not be computed.
    size_t maxLinearIndex() const noexcept { return m_max_index; }

    void buildWarpTables(int scale_code, double fmax, size_t n_out, double nyquist, double warp_blend, double log_floor_hz, size_t nlin) {
        computeTargetHzGrid(scale_code, fmax, n_out, warp_blend, log_floor_hz, m_target_hz);
        m_i0.resize(n_out);
        m_w.resize(n_out);
        m_nlin = nlin;
        m_max_index = 0;
        double denom = (nlin > 1) ? static_cast<double>(nlin - 1) : 1.0;
        bool is_id = (n_out == nlin);
        const size_t max_i0 = (nlin >= 2) ? nlin - 2 : 0;
        for (size_t i = 0; i < n_out; ++i) {
            double frac = (nyquist > 0.0) ? (m_target_hz[i] / nyquist) * denom : 0.0;
            // Snap positions that are an integer up to rounding noise, so an exact 1:1 grid
            // (Linear scale with bins == linear bins) is detected as identity (memcpy bypass).
            double rounded = std::round(frac);
            if (std::abs(frac - rounded) < 1e-6) frac = rounded;
            size_t i0_val = static_cast<size_t>(std::max(0.0, std::min(static_cast<double>(max_i0), std::floor(frac))));
            float weight = static_cast<float>(std::clamp(frac - static_cast<double>(i0_val), 0.0, 1.0));
            m_i0[i] = static_cast<uint32_t>(i0_val);
            m_w[i] = weight;
            m_max_index = std::max(m_max_index, std::min(nlin - 1, i0_val + 2));
            // identity iff every output bin samples exactly linear bin i (the last bin is
            // represented as i0 = nlin-2 with weight 1.0 because of the clamp above)
            if (is_id && std::abs((static_cast<double>(i0_val) + weight) - static_cast<double>(i)) > 1e-5) is_id = false;
        }
        m_is_identity = is_id;
    }

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
        if (m_interp == 1) {
            applyWarpCubic(src, dst, idx, w_ptr, n_out, linear_magnitude.size());
            return;
        }
#if defined(__AVX2__)
        // Guard: gathers index src[i0+1]; tables guarantee i0 <= nlin-2 <= src.size()-2.
        if (linear_magnitude.size() >= m_nlin) {
            for (; i + 7 < n_out; i += 8) {
                __m256i vi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(idx + i));
                __m256 v0 = _mm256_i32gather_ps(src, vi, 4);
                __m256 v1 = _mm256_i32gather_ps(src + 1, vi, 4);
                __m256 w = _mm256_loadu_ps(w_ptr + i);
                _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(w, _mm256_sub_ps(v1, v0), v0));
            }
        }
#endif
        for (; i < n_out; ++i) {
            size_t i0 = idx[i];
            float w = w_ptr[i];
            dst[i] = src[i0] + w * (src[i0 + 1] - src[i0]);
        }
    }

    // Catmull-Rom: v = 0.5*(2p1 + (-p0+p2)t + (2p0-5p1+4p2-p3)t^2 + (-p0+3p1-3p2+p3)t^3), clamped >= 0
    void applyWarpCubic(const float* __restrict src, float* __restrict dst, const uint32_t* __restrict idx,
                        const float* __restrict w_ptr, size_t n_out, size_t src_size) const noexcept {
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
        for (; i + 7 < n_out; i += 8) {
            __m256i vi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(idx + i));
            __m256i im1 = _mm256_max_epi32(_mm256_sub_epi32(vi, one), v_zero);
            __m256i ip1 = _mm256_min_epi32(_mm256_add_epi32(vi, one), v_last);
            __m256i ip2 = _mm256_min_epi32(_mm256_add_epi32(vi, two), v_last);
            __m256 p0 = _mm256_i32gather_ps(src, im1, 4);
            __m256 p1 = _mm256_i32gather_ps(src, vi, 4);
            __m256 p2 = _mm256_i32gather_ps(src, ip1, 4);
            __m256 p3 = _mm256_i32gather_ps(src, ip2, 4);
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

    const std::vector<double>& targetHz() const noexcept { return m_target_hz; }
    bool isIdentity() const noexcept { return m_is_identity; }
    size_t outputBins() const noexcept { return m_i0.size(); }

private:
    std::vector<double> m_target_hz;
    std::vector<uint32_t> m_i0;
    std::vector<float> m_w;
    size_t m_nlin{ 0 };
    size_t m_max_index{ 0 };
    int m_interp{ 0 };
    bool m_is_identity{ false };
};

/*
===========================================================================
 5. EQUAL-LOUDNESS WEIGHTING CURVES (IEC 61672 / ITU-R 468)
===========================================================================
*/
class EqualLoudness {
public:
    // weighting_code: 0 off, 1 A, 2 C, 3 ITU-R 468
    static void computeCurve(int weighting_code, const std::vector<double>& freqs_hz, AlignedVector& weights) {
        weights.resize(freqs_hz.size());
        if (weighting_code == 0) { std::fill(weights.begin(), weights.end(), 1.0f); return; }
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
            double f = std::max(1e-5, freqs_hz[i]);
            double w = 1.0;
            if (weighting_code == 1) {
                double f2 = f * f;
                double num = (12194.0 * 12194.0) * (f2 * f2);
                double den = (f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0);
                w = (num / std::max(1e-12, den)) * inv_ref;
            } else if (weighting_code == 2) {
                double f2 = f * f;
                double num = (12194.0 * 12194.0) * f2;
                double den = (f2 + 20.6 * 20.6) * (f2 + 12194.0 * 12194.0);
                w = (num / std::max(1e-12, den)) * inv_ref;
            } else if (weighting_code == 3) {
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
Max error 20*log10(1 + 1/2048) = 0.0042 dB (the earlier 256-entry table was 0.034 dB).
One gather per 8 bins: an interpolated two-gather variant measured 2x slower in the
dB stage for no visible benefit on a display spectrum. The 8 KB table stays L1-resident.
Table is a function-local static -> thread-safe initialisation.
*/
class FastLog10 {
public:
    static constexpr float kLog10_2_Scaled = 6.020599913282299f; // 20 * log10(2)
    static constexpr int kTableBits = 11;
    static constexpr int kTableSize = 1 << kTableBits;             // 2048 entries (8 KB)

    static const float* dbTable() noexcept {
        static const struct Table {
            float v[kTableSize];
            Table() {
                for (int i = 0; i < kTableSize; ++i)
                    v[i] = 20.0f * std::log10(1.0f + (static_cast<float>(i) + 0.5f) / static_cast<float>(kTableSize)); // mid-interval
            }
        } table;
        return table.v;
    }

    // Scalar 20*log10(x) for positive normal x. Pass the table pointer from dbTable() when calling in a loop.
    static float scaled(float x, const float* t) noexcept {
        uint32_t bits;
        std::memcpy(&bits, &x, sizeof(bits));
        int exp = static_cast<int>((bits >> 23) & 0xFF) - 127;
        int idx = static_cast<int>((bits >> (23 - kTableBits)) & (kTableSize - 1));
        return exp * kLog10_2_Scaled + t[idx];
    }
    static float scaled(float x) noexcept { return scaled(x, dbTable()); }

#if defined(__AVX2__)
    static inline __m256 scaledVec(__m256 v, const float* t) noexcept {
        const __m256i bits = _mm256_castps_si256(v);
        const __m256i exp = _mm256_sub_epi32(_mm256_and_si256(_mm256_srli_epi32(bits, 23), _mm256_set1_epi32(0xFF)), _mm256_set1_epi32(127));
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
*/
// dst[i] = a[i] * b[i]. a and b must be 32-byte aligned; dst may be unaligned.
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

// Maximum value (data 32-byte aligned)
inline float peakMagnitude(const float* __restrict data, size_t n) noexcept {
    if (n == 0) return 0.0f;
    float max_val = data[0];
    size_t i = 0;
#if defined(__AVX2__)
    __m256 v_max = _mm256_set1_ps(max_val);
    for (; i + 15 < n; i += 16) {
        v_max = _mm256_max_ps(v_max, _mm256_max_ps(_mm256_load_ps(data + i), _mm256_load_ps(data + i + 8)));
    }
    for (; i + 7 < n; i += 8) v_max = _mm256_max_ps(v_max, _mm256_load_ps(data + i));
    alignas(32) float tmp[8];
    _mm256_store_ps(tmp, v_max);
    for (int k = 0; k < 8; ++k) max_val = std::max(max_val, tmp[k]);
#endif
    for (; i < n; ++i) max_val = std::max(max_val, data[i]);
    return max_val;
}

// Peak value and index (data 32-byte aligned). Vector loop starts at 0 (aligned) and only
// inspects a chunk when some lane exceeds the running max.
inline float findPeakWithIndex(const float* __restrict data, size_t n, size_t& peak_idx) noexcept {
    peak_idx = 0;
    if (n == 0) return 0.0f;
    float max_val = data[0];
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 7 < n; i += 8) {
        __m256 v = _mm256_load_ps(data + i);
        __m256 gt = _mm256_cmp_ps(v, _mm256_set1_ps(max_val), _CMP_GT_OQ);
        if (_mm256_movemask_ps(gt) != 0) {
            alignas(32) float tmp[8];
            _mm256_store_ps(tmp, v);
            for (int j = 0; j < 8; ++j) {
                if (tmp[j] > max_val) { max_val = tmp[j]; peak_idx = i + j; }
            }
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
 6c. DECIBEL CONVERSION
===========================================================================
mode: 1 = dB, 2 = dB normalised to [0,1] over [-top_db, 0]
inv_ref: 1 / reference magnitude (0 dB point). The caller chooses the reference
(frame peak, absolute full scale, or an AGC follower).
*/
class DecibelConverter {
public:
    static inline void convertToDB(int mode, double top_db, float inv_ref, AlignedVector& spectrum) noexcept {
        if (mode == 0 || spectrum.empty()) return;
        size_t n = spectrum.size();
        float* data = spectrum.data();
        if (!(inv_ref > 0.0f) || !std::isfinite(inv_ref)) inv_ref = 1.0f;

        const float* lut = FastLog10::dbTable();   // hoisted: one static-init guard check per call, not per 8 bins
        const float db_offset = FastLog10::scaled(inv_ref, lut);
        const float floor_val = static_cast<float>(-top_db);
        const float inv_top_db = static_cast<float>(1.0 / std::max(1e-6, top_db));
        const float kMinMag = 1e-12f;

        size_t k = 0;
#if defined(__AVX2__)
        const __m256 min_v = _mm256_set1_ps(kMinMag);
        const __m256 db_off_v = _mm256_set1_ps(db_offset);
        const __m256 floor_v = _mm256_set1_ps(floor_val);
        if (mode == 2) {
            const __m256 zero_v = _mm256_setzero_ps();
            const __m256 one_v = _mm256_set1_ps(1.0f);
            const __m256 inv_top_db_v = _mm256_set1_ps(inv_top_db);
            for (; k + 15 < n; k += 16) {
                __m256 v0 = _mm256_max_ps(_mm256_load_ps(data + k), min_v);
                __m256 v1 = _mm256_max_ps(_mm256_load_ps(data + k + 8), min_v);
                __m256 db0 = _mm256_max_ps(_mm256_add_ps(FastLog10::scaledVec(v0, lut), db_off_v), floor_v);
                __m256 db1 = _mm256_max_ps(_mm256_add_ps(FastLog10::scaledVec(v1, lut), db_off_v), floor_v);
                __m256 n0 = _mm256_mul_ps(_mm256_sub_ps(db0, floor_v), inv_top_db_v);
                __m256 n1 = _mm256_mul_ps(_mm256_sub_ps(db1, floor_v), inv_top_db_v);
                _mm256_store_ps(data + k,     _mm256_min_ps(_mm256_max_ps(zero_v, n0), one_v));
                _mm256_store_ps(data + k + 8, _mm256_min_ps(_mm256_max_ps(zero_v, n1), one_v));
            }
            for (; k + 7 < n; k += 8) {
                __m256 v = _mm256_max_ps(_mm256_load_ps(data + k), min_v);
                __m256 db = _mm256_max_ps(_mm256_add_ps(FastLog10::scaledVec(v, lut), db_off_v), floor_v);
                __m256 nn = _mm256_mul_ps(_mm256_sub_ps(db, floor_v), inv_top_db_v);
                _mm256_store_ps(data + k, _mm256_min_ps(_mm256_max_ps(zero_v, nn), one_v));
            }
        } else {
            for (; k + 15 < n; k += 16) {
                __m256 v0 = _mm256_max_ps(_mm256_load_ps(data + k), min_v);
                __m256 v1 = _mm256_max_ps(_mm256_load_ps(data + k + 8), min_v);
                _mm256_store_ps(data + k,     _mm256_max_ps(_mm256_add_ps(FastLog10::scaledVec(v0, lut), db_off_v), floor_v));
                _mm256_store_ps(data + k + 8, _mm256_max_ps(_mm256_add_ps(FastLog10::scaledVec(v1, lut), db_off_v), floor_v));
            }
            for (; k + 7 < n; k += 8) {
                __m256 v = _mm256_max_ps(_mm256_load_ps(data + k), min_v);
                _mm256_store_ps(data + k, _mm256_max_ps(_mm256_add_ps(FastLog10::scaledVec(v, lut), db_off_v), floor_v));
            }
        }
#endif
        for (; k < n; ++k) {
            float raw_v = std::max(kMinMag, data[k]);
            float db = std::max(FastLog10::scaled(raw_v, lut) + db_offset, floor_val);
            data[k] = (mode == 2) ? std::max(0.0f, std::min(1.0f, (db - floor_val) * inv_top_db)) : db;
        }
    }
};

/*
===========================================================================
 7. ASYMMETRIC ATTACK / RELEASE BALLISTICS FILTER
===========================================================================
attack/release are per-frame smoothing coefficients in [0, 0.99]
(0 = follow instantly). Use coefFromMs() for frame-rate independent values.
*/
class BallisticsFilter {
public:
    // Time constant (ms) -> per-frame coefficient for the given frame delta (ms). tau = time to reach 63%.
    static float coefFromMs(double time_ms, double dt_ms) noexcept {
        if (time_ms <= 0.0 || dt_ms <= 0.0) return 0.0f;
        return static_cast<float>(std::clamp(std::exp(-dt_ms / time_ms), 0.0, 0.999));
    }

    inline void apply(float attack, float release, const AlignedVector& current, AlignedVector& prev_out) noexcept {
        size_t n = current.size();
        if (prev_out.size() != n || (attack <= 0.0f && release <= 0.0f)) { prev_out = current; return; }
        float att_factor = 1.0f - std::clamp(attack, 0.0f, 0.999f);
        float rel_factor = 1.0f - std::clamp(release, 0.0f, 0.999f);
        const float* src = current.data();
        float* dst = prev_out.data();
        size_t i = 0;
#if defined(__AVX2__)
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
        for (; i < n; ++i) {
            float diff = src[i] - dst[i];
            dst[i] += ((diff > 0.0f) ? att_factor : rel_factor) * diff;
        }
    }
};

/*
===========================================================================
 8. FFT ENGINE INTERFACE & AVX2 MAGNITUDE
===========================================================================
*/
enum class PlannerPolicy : int {
    Auto = 0,     // MEASURE for N <= 16384, ESTIMATE above (never a noticeable stall)
    Fast = 1,     // ESTIMATE always
    Measured = 2, // MEASURE always; plans are cached in wisdom so only the first run of a size costs time
};

class IFFTEngine {
public:
    virtual ~IFFTEngine() = default;
    virtual void prepare(size_t fft_size, PlannerPolicy policy, PlanLog* log) = 0;
    // n_mag: number of magnitude bins to compute (0 = all N/2+1). Bins above n_mag are left untouched.
    virtual void executeRFFT(const AlignedVector& padded_signal, AlignedVector& magnitude_spectrum,
                             AlignedComplexVector& scratch_complex, size_t n_mag = 0) const noexcept = 0;
    virtual std::string getPlanStatus() const = 0;
    virtual size_t fftSize() const noexcept = 0;
    // Called once per cook on the cooking thread; returns true if a better plan was swapped in.
    virtual bool pollBackgroundPlan() { return false; }
};

// |X| for n_complex interleaved complex floats (raw_c and mptr 32-byte aligned).
inline void computeMagnitudeAVX2_FMA(const float* __restrict raw_c, float* __restrict mptr, size_t n_complex) noexcept {
    size_t i = 0;
#if defined(__AVX2__)
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
 9. FFTW3 ENGINE (single precision, wisdom-cached plans)
===========================================================================
Wisdom is imported once per process from %LOCALAPPDATA%/TD_Custom_FFT/fftwf_wisdom.txt and
exported after every MEASURE plan, so measured plans cost time only the first time a size
is used on this machine. The FFTW planner is not thread-safe: prepare() is guarded by a
process-wide mutex; fftwf_execute_dft_r2c on distinct arrays is thread-safe.
*/
class FFTWEngine : public IFFTEngine {
public:
    FFTWEngine() = default;
    ~FFTWEngine() override {
        joinBackground();
        destroyPlan();
    }
    FFTWEngine(const FFTWEngine&) = delete;
    FFTWEngine& operator=(const FFTWEngine&) = delete;

    static std::mutex& plannerMutex() { static std::mutex m; return m; }

    // Optional override of the wisdom file location (tests, portable installs). Empty = default.
    static std::string& wisdomPathOverride() { static std::string s; return s; }

    static std::string wisdomPath() {
        if (!wisdomPathOverride().empty()) return wisdomPathOverride();
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
    }

    static bool importWisdomOnce(PlanLog* log) {
        static bool done = false;
        static bool ok = false;
        if (done) return ok;
        done = true;
        std::string path = wisdomPath();
        if (!path.empty()) {
            ok = fftwf_import_wisdom_from_filename(path.c_str()) != 0;
            if (log) log->log(std::string("[FFT Plugin] [FFTW3] wisdom ") + (ok ? "loaded from " : "not found at ") + path, ok);
        }
        return ok;
    }

    static void exportWisdom() {
        std::string path = wisdomPath();
        if (!path.empty()) fftwf_export_wisdom_to_filename(path.c_str());
    }

    void destroyPlan() noexcept {
        joinBackground();
        if (m_plan) {
            std::lock_guard<std::mutex> lock(plannerMutex());
            fftwf_destroy_plan(m_plan);
            m_plan = nullptr;
        }
        m_fft_size = 0;
        m_planStatus = "FFTW3 (Uninitialized)";
    }

    std::string getPlanStatus() const override { return m_planStatus.empty() ? "FFTW3 (Uninitialized)" : m_planStatus; }
    size_t fftSize() const noexcept override { return m_fft_size; }
    bool upgradeInProgress() const noexcept { return m_bg_running.load(); }

    /*
      Planner policies
        Fast     : FFTW_ESTIMATE only (never stalls, generic plan).
        Measured : FFTW_MEASURE synchronously (best plan; ~0.4 s once per size per machine, then cached in wisdom).
        Auto     : if wisdom already holds a measured plan for this size (FFTW_MEASURE | FFTW_WISDOM_ONLY) use it
                   instantly; otherwise use an ESTIMATE plan right away and measure a better one on a background
                   thread, swap it in on the next cook (pollBackgroundPlan) and save it to wisdom. Real-time is never
                   interrupted and the second run of any size is already optimal.
    */
    void prepare(size_t fft_size, PlannerPolicy policy, PlanLog* log) override {
        if (m_fft_size == fft_size && m_plan != nullptr && m_policy == policy) return;
        destroyPlan();
        if (fft_size == 0) return;
        m_fft_size = fft_size;
        m_policy = policy;
        m_log = log;

        importWisdomOnce(log);

        auto t0 = std::chrono::high_resolution_clock::now();
        const char* used = "FFTW_ESTIMATE";
        bool start_background = false;
        {
            std::lock_guard<std::mutex> lock(plannerMutex());
            Buffers b(fft_size);
            if (!b.ok()) { m_planStatus = "FFTW3 (allocation failed)"; return; }
            switch (policy) {
                case PlannerPolicy::Measured:
                    m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), b.in, b.out, FFTW_MEASURE);
                    used = "FFTW_MEASURE";
                    if (m_plan) exportWisdom();
                    break;
                case PlannerPolicy::Fast:
                    m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), b.in, b.out, FFTW_ESTIMATE);
                    break;
                case PlannerPolicy::Auto:
                default:
                    m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), b.in, b.out, FFTW_MEASURE | FFTW_WISDOM_ONLY);
                    if (m_plan) {
                        used = "FFTW_MEASURE (from wisdom)";
                    } else {
                        m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), b.in, b.out, FFTW_ESTIMATE);
                        used = "FFTW_ESTIMATE (measuring in background)";
                        start_background = true;
                    }
                    break;
            }
            if (!m_plan && policy != PlannerPolicy::Fast) {
                m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), b.in, b.out, FFTW_ESTIMATE);
                used = "FFTW_ESTIMATE (fallback)";
            }
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        if (m_plan) {
            m_planStatus = "FFTW3 (" + std::string(used) + " - " + std::to_string(ms) + " ms, N=" + std::to_string(fft_size) + ")";
            if (log) log->log("[FFT Plugin] [FFTW3] plan N=" + std::to_string(fft_size) + " " + used + " in " + std::to_string(ms) + " ms");
        } else {
            m_planStatus = "FFTW3 (plan creation FAILED)";
            if (log) log->log("[FFT Plugin] [FFTW3] ERROR: plan creation failed for N=" + std::to_string(fft_size));
            return;
        }
        if (start_background) startBackgroundMeasure(fft_size);
    }

    // Call once per cook from the cooking thread (before any channel executes). Swaps in a
    // background-measured plan when one is ready. Returns true when the plan changed.
    bool pollBackgroundPlan() override {
        fftwf_plan ready = m_bg_plan.exchange(nullptr);
        if (!ready) return false;
        if (m_bg_size != m_fft_size) {                 // size changed meanwhile: discard
            std::lock_guard<std::mutex> lock(plannerMutex());
            fftwf_destroy_plan(ready);
            joinBackground();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(plannerMutex());
            if (m_plan) fftwf_destroy_plan(m_plan);
            m_plan = ready;
        }
        joinBackground();
        m_planStatus = "FFTW3 (FFTW_MEASURE upgraded in background - " + std::to_string(m_bg_ms) + " ms, N=" + std::to_string(m_fft_size) + ")";
        if (m_log) m_log->log("[FFT Plugin] [FFTW3] background FFTW_MEASURE plan ready for N=" + std::to_string(m_fft_size) + " (" + std::to_string(m_bg_ms) + " ms), swapped in");
        return true;
    }

    void executeRFFT(const AlignedVector& padded_signal, AlignedVector& magnitude_spectrum,
                     AlignedComplexVector& scratch_complex, size_t n_mag = 0) const noexcept override {
        size_t n = padded_signal.size();
        size_t n_complex = n / 2 + 1;
        if (magnitude_spectrum.size() != n_complex) magnitude_spectrum.resize(n_complex);
        if (scratch_complex.size() != n_complex) scratch_complex.resize(n_complex);
        if (m_plan && n == m_fft_size) {
            float* in_ptr = const_cast<float*>(padded_signal.data());
            fftwf_complex* out_ptr = reinterpret_cast<fftwf_complex*>(scratch_complex.data());
            fftwf_execute_dft_r2c(m_plan, in_ptr, out_ptr);
        } else {
            std::memset(scratch_complex.data(), 0, n_complex * sizeof(std::complex<float>));
        }
        size_t count = (n_mag == 0) ? n_complex : std::min(n_complex, ((n_mag + 15) / 16) * 16);   // keep 16-bin SIMD blocks
        computeMagnitudeAVX2_FMA(reinterpret_cast<const float*>(scratch_complex.data()), magnitude_spectrum.data(), std::min(count, n_complex));
    }

private:
    struct Buffers {
        float* in{ nullptr }; fftwf_complex* out{ nullptr };
        explicit Buffers(size_t n) {
            in = static_cast<float*>(fftwf_malloc(sizeof(float) * n));
            out = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * (n / 2 + 1)));
        }
        ~Buffers() { if (in) fftwf_free(in); if (out) fftwf_free(out); }
        bool ok() const { return in && out; }
    };

    void startBackgroundMeasure(size_t fft_size) {
        joinBackground();
        m_bg_size = fft_size;
        m_bg_running = true;
        m_bg_thread = std::thread([this, fft_size]() {
            auto t0 = std::chrono::high_resolution_clock::now();
            fftwf_plan p = nullptr;
            {
                std::lock_guard<std::mutex> lock(plannerMutex());   // planner is single-threaded; execute() is thread-safe
                Buffers b(fft_size);
                if (b.ok()) p = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), b.in, b.out, FFTW_MEASURE);
                if (p) exportWisdom();
            }
            m_bg_ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
            m_bg_plan.store(p);
            m_bg_running = false;
        });
    }

    void joinBackground() noexcept {
        if (m_bg_thread.joinable()) {
            try { m_bg_thread.join(); } catch (...) {}
        }
        fftwf_plan leftover = m_bg_plan.exchange(nullptr);
        if (leftover) {
            std::lock_guard<std::mutex> lock(plannerMutex());
            fftwf_destroy_plan(leftover);
        }
        m_bg_running = false;
    }

    fftwf_plan m_plan{ nullptr };
    size_t m_fft_size{ 0 };
    PlannerPolicy m_policy{ PlannerPolicy::Auto };
    std::string m_planStatus{ "FFTW3 (Uninitialized)" };
    PlanLog* m_log{ nullptr };

    std::thread m_bg_thread;
    std::atomic<fftwf_plan> m_bg_plan{ nullptr };
    std::atomic<bool> m_bg_running{ false };
    size_t m_bg_size{ 0 };
    double m_bg_ms{ 0.0 };
};

} // namespace FFTDSP

#endif // DSP_MODULES_H
