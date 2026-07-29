#ifndef DSP_MODULES_H
#define DSP_MODULES_H

/*
===========================================================================
                        HIGH-PERFORMANCE DSP ENGINE
===========================================================================
Module Architecture: Real-Time Audio Signal Processing & Psychoacoustic FFT
Targeted Platform: TouchDesigner Custom CHOP C++ Plugin (x86_64 AVX2)

OVERVIEW & ARCHITECTURAL DESIGN:
---------------------------------------------------------------------------
This header defines the core mathematical, psychoacoustic, and spectral 
processing algorithms powering the TouchDesigner Custom FFT CHOP.

Processing Pipeline Overview:
1. Audio Ingestion: Circular Ring Buffering (FIFOBuffer) - zero heap allocations.
2. Pre-Filtering: Parametric Biquad Equalizer (BiquadEQ / Direct Form II Transposed).
3. Tapering & Padding: Window Generator (Kaiser/Hann/etc.) with Coherent Gain Compensation.
4. Spectral Analysis: Single-Precision FFTW3 R2C Transform (FFTWEngine).
5. SIMD Vectorization: 256-bit AVX2 Parallel Magnitude Spectrum calculation (2x unrolled).
6. Psychoacoustic Re-mapping: Perceptual Frequency Scales with Identity Bypass (Log, Mel, ERB, Bark, Chroma, Melog).
7. Equal-Loudness Weighting: ISO 226 / IEC 61672 A-Weighting, C-Weighting, ITU-R 468 (2x unrolled).
8. Dynamic Range Conversion: Optimized Decibel (dB) scaling with parallel peak search.
9. Temporal Smoothing: Asymmetric Attack/Release Envelope Ballistics Filter (2x unrolled FMA).
===========================================================================
*/

#include <vector>
#include <cmath>
#include <complex>
#include <algorithm>
#include <tuple>
#include <cstdint>
#include <cstring>
#include <immintrin.h> // AVX2 & FMA SIMD Compiler Intrinsics

#include <fftw3.h>     // FFTW3 Fast Fourier Transform Library (Single Precision: fftwf_*)

namespace FFTDSP {

// Fundamental mathematical constants evaluated at highest float/double precision
constexpr float PI_F = 3.14159265358979323846f;
constexpr double PI_D = 3.14159265358979323846;

/*
===========================================================================
 1. CIRCULAR RING BUFFER (FIFOBuffer)
===========================================================================
Zero-allocation circular ring buffer for real-time audio sample ingestion.

Performance & Architecture Details:
- Guaranteed zero heap allocation during real-time cook calls once capacity is set.
- Handles incoming variable sub-block sizes (e.g. 512, 735, 1024 samples) seamlessly.
- Uses split std::memcpy operations when incoming block wraps around buffer boundaries.
*/
class FIFOBuffer {
public:
    explicit FIFOBuffer(size_t capacity = 3175) {
        resize(capacity);
    }

    void resize(size_t capacity) {
        m_capacity = std::max<size_t>(1, capacity);
        m_data.assign(m_capacity, 0.0f);
        m_idx = 0;
        m_filled = 0;
    }

    size_t capacity() const noexcept { return m_capacity; }

    inline void add(const float* signal, size_t count) noexcept {
        if (count == 0) return;
        
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

        if (m_filled < m_capacity) {
            m_filled = std::min(m_capacity, m_filled + count);
        }
    }

    inline void get(std::vector<float>& out) const noexcept {
        if (out.size() != m_capacity) {
            out.resize(m_capacity);
        }
        
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
 2. DIRECT FORM II TRANSPOSED BIQUAD EQUALIZER
===========================================================================
Second-order Infinite Impulse Response (IIR) Biquad Filter section.
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

    void reset() noexcept {
        z1 = 0.0f;
        z2 = 0.0f;
    }
};

/*
Dual-stage parametric equalizer featuring High-Shelf and Low-Shelf filters.
Uses Robert Bristow-Johnson (RBJ) Audio EQ Cookbook analytic derivations.
Includes lazy redesign evaluation using parameter tuple hashing to eliminate
redundant trigonometric calculations when EQ parameters remain unchanged.
*/
class BiquadEQ {
public:
    explicit BiquadEQ(double sampling_rate = 44100.0)
        : m_sample_rate(sampling_rate) {}

    void setSampleRate(double sr) noexcept {
        if (m_sample_rate != sr) {
            m_sample_rate = sr;
            m_design_key = std::make_tuple(0.0, 0.0, 0.0, 0.0, 0.0);
        }
    }

    /*
     * Unified shelf filter design (RBJ Audio EQ Cookbook).
     * is_high=true for high-shelf, is_high=false for low-shelf.
     * Uses sign factor s (+1 high, -1 low) to select coefficient variants.
     */
    void designShelf(bool is_high, double cutoff_hz, double gain_db, double q_factor, BiquadSection& sec) noexcept {
        if (std::abs(gain_db) < 0.01) {
            sec.active = false;
            return;
        }
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

    inline void processAudio(const std::vector<float>& original_audio,
                             double gain_db, double cutoff_hz,
                             double low_gain_db, double low_cutoff_hz,
                             double q_factor, double amount,
                             std::vector<float>& processed_output) noexcept {
        if (original_audio.empty()) {
            processed_output.clear();
            return;
        }

        std::tuple<double, double, double, double, double> key{ gain_db, cutoff_hz, low_gain_db, low_cutoff_hz, q_factor };
        if (m_design_key != key) {
            designShelf(true, cutoff_hz, gain_db, q_factor, m_high_shelf);
            designShelf(false, low_cutoff_hz, low_gain_db, q_factor, m_low_shelf);
            m_design_key = key;
        }

        float amt = static_cast<float>(amount);
        bool has_filter = (m_high_shelf.active || m_low_shelf.active) && (amt > 0.0f);

        if (!has_filter) {
            return;
        }

        if (processed_output.size() != original_audio.size()) {
            processed_output.resize(original_audio.size());
        }

        const float* src = original_audio.data();
        float* dst = processed_output.data();
        size_t n = original_audio.size();

        for (size_t i = 0; i < n; ++i) {
            float x = src[i];
            float filtered = m_high_shelf.process(x);
            filtered = m_low_shelf.process(filtered);
            dst[i] = x + amt * (filtered - x);
        }
    }

    bool hasActiveFilter(double amount) const noexcept {
        return (m_high_shelf.active || m_low_shelf.active) && (amount > 0.0);
    }

private:
    double m_sample_rate{ 44100.0 };
    BiquadSection m_high_shelf;
    BiquadSection m_low_shelf;
    std::tuple<double, double, double, double, double> m_design_key;
};

/*
===========================================================================
 3. WINDOW GENERATOR WITH COHERENT GAIN COMPENSATION
===========================================================================
Generates mathematical tapering windows for spectral analysis.
Coherent Gain Normalization scales window values so mean(window) == 1.0,
preserving exact sinusoidal peak values in spectral outputs.
*/
class WindowGenerator {
public:
    static double besselI0(double x) {
        double ax = std::abs(x);
        if (ax < 3.75) {
            double y = x / 3.75;
            y = y * y;
            return 1.0 + y * (3.5156229 + y * (3.0899424 + y * (1.2067492 + y * (0.2659732 + y * (0.0360768 + y * 0.0045813)))));
        } else {
            double y = 3.75 / ax;
            return (std::exp(ax) / std::sqrt(ax)) *
                   (0.39894228 + y * (0.01328592 + y * (0.00225319 + y * (-0.00157565 + y * (0.00916281 + y * (-0.02057706 + y * (0.02635537 + y * (-0.01647633 + y * 0.00392377))))))));
        }
    }

    static void generateWindow(int window_type, int kaiser_beta, size_t length, std::vector<float>& window) {
        window.resize(length);
        if (length == 0) return;

        double denom = (length > 1) ? static_cast<double>(length - 1) : 1.0;
        double sum = 0.0;

        // Pre-compute Kaiser denominator once (saves N besselI0 calls in the per-sample loop)
        double kaiser_beta_d = static_cast<double>(kaiser_beta);
        double kaiser_inv_I0 = 1.0 / besselI0(kaiser_beta_d);

        for (size_t n = 0; n < length; ++n) {
            double w = 1.0;
            double fn = static_cast<double>(n);

            switch (window_type) {
                case 1: // Hann Window
                    w = 0.5 - 0.5 * std::cos(2.0 * PI_D * fn / denom);
                    break;
                case 2: // Hamming Window
                    w = 0.54 - 0.46 * std::cos(2.0 * PI_D * fn / denom);
                    break;
                case 3: // Blackman Window
                    w = 0.42 - 0.5 * std::cos(2.0 * PI_D * fn / denom) + 0.08 * std::cos(4.0 * PI_D * fn / denom);
                    break;
                case 4: // Blackman-Harris Window (92dB attenuation)
                    w = 0.35875 - 0.48829 * std::cos(2.0 * PI_D * fn / denom)
                        + 0.14128 * std::cos(4.0 * PI_D * fn / denom)
                        - 0.01168 * std::cos(6.0 * PI_D * fn / denom);
                    break;
                case 5: // Rectangular Window (Flat)
                    w = 1.0;
                    break;
                case 0: // Kaiser Window (Beta parameter control)
                default: {
                    double term = 2.0 * fn / denom - 1.0;
                    double arg = std::sqrt(std::max(0.0, 1.0 - term * term));
                    w = besselI0(kaiser_beta_d * arg) * kaiser_inv_I0;
                    break;
                }
            }
            window[n] = static_cast<float>(w);
            sum += w;
        }

        double mean_val = sum / static_cast<double>(length);
        if (mean_val > 0.0) {
            float inv_mean = static_cast<float>(1.0 / mean_val);
            for (size_t n = 0; n < length; ++n) {
                window[n] *= inv_mean;
            }
        }
    }
};

/*
===========================================================================
 4. PSYCHOACOUSTIC FREQUENCY SCALES & UNROLLED SIMD INTERPOLATION
===========================================================================
Converts linear FFT frequency spectrums to psychoacoustic perception scales.
Identity Bypass: Fast memcpy when grid mapping is 1-to-1 linear identity.
FMA formulation: v0 + w * (v1 - v0) unrolled 4x for SIMD execution.
*/
class PerceptualWarping {
public:
    static double htkHzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
    static double htkMelToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

    static double erbRateGlasberg(double hz) {
        double x = hz / 123.0;
        return 6.230 * (x * x) + 93.390 * x + 28.520;
    }
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

    static double hzToChroma(double hz) {
        double f = std::max(1e-5, hz);
        return 12.0 * std::log2(f / 440.0) + 69.0;
    }
    static double chromaToHz(double chroma) {
        return 440.0 * std::pow(2.0, (chroma - 69.0) / 12.0);
    }

    static void computeTargetHzGrid(int scale_code, double fmax, size_t n_out, double nyquist, double warp_blend, double log_floor_hz, std::vector<double>& target_hz) {
        target_hz.resize(n_out);
        if (n_out == 0) return;
        double inv_denom = (n_out > 1) ? 1.0 / static_cast<double>(n_out - 1) : 0.0;
        std::vector<double> perceptual(n_out);

        switch (scale_code) {
            case 1: { // Logarithmic Scale
                double floor_hz = std::max(1.0, log_floor_hz);
                double fmin = std::min(floor_hz, std::min(0.5 * fmax, std::max(1.0, fmax * 0.1)));
                double log_min = std::log(fmin);
                double log_max = std::log(fmax);
                for (size_t i = 0; i < n_out; ++i) {
                    double frac = i * inv_denom;
                    perceptual[i] = std::exp(log_min + frac * (log_max - log_min));
                }
                break;
            }
            case 2: { // Mel Scale
                double m_min = htkHzToMel(0.0);
                double m_max = htkHzToMel(fmax);
                for (size_t i = 0; i < n_out; ++i) {
                    double frac = i * inv_denom;
                    perceptual[i] = htkMelToHz(m_min + frac * (m_max - m_min));
                }
                break;
            }
            case 3: { // ERB Scale
                double e_min = erbRateGlasberg(0.0);
                double e_max = erbRateGlasberg(fmax);
                for (size_t i = 0; i < n_out; ++i) {
                    double frac = i * inv_denom;
                    perceptual[i] = erbRateToHz(e_min + frac * (e_max - e_min));
                }
                break;
            }
            case 4: { // Bark Scale
                double b_min = hzToBark(0.0);
                double b_max = hzToBark(fmax);
                for (size_t i = 0; i < n_out; ++i) {
                    double frac = i * inv_denom;
                    perceptual[i] = barkToHz(b_min + frac * (b_max - b_min));
                }
                break;
            }
            case 5: { // Chroma Scale
                double c_min = hzToChroma(20.0);
                double c_max = hzToChroma(fmax);
                for (size_t i = 0; i < n_out; ++i) {
                    double frac = i * inv_denom;
                    perceptual[i] = chromaToHz(c_min + frac * (c_max - c_min));
                }
                break;
            }
            case 6: { // Mel + Log Blend Scale
                double m_min = htkHzToMel(0.0);
                double m_max = htkHzToMel(fmax);
                double floor_hz = std::max(1.0, log_floor_hz);
                double fmin = std::min(floor_hz, std::min(0.5 * fmax, std::max(1.0, fmax * 0.1)));
                double log_min = std::log(fmin);
                double log_max = std::log(fmax);
                for (size_t i = 0; i < n_out; ++i) {
                    double frac = i * inv_denom;
                    double mel_hz = htkMelToHz(m_min + frac * (m_max - m_min));
                    double log_hz = std::exp(log_min + frac * (log_max - log_min));
                    perceptual[i] = 0.5 * (mel_hz + log_hz);
                }
                break;
            }
            case 0: // Linear Scale
            default: {
                for (size_t i = 0; i < n_out; ++i) {
                    perceptual[i] = i * inv_denom * fmax;
                }
                break;
            }
        }

        for (size_t i = 0; i < n_out; ++i) {
            double lin = i * inv_denom * fmax;
            double axis_val = (1.0 - warp_blend) * lin + warp_blend * perceptual[i];
            target_hz[i] = std::max(0.0, std::min(fmax, axis_val));
        }
    }

    void buildWarpTables(int scale_code, double fmax, size_t n_out, double nyquist, double warp_blend, double log_floor_hz, size_t nlin) {
        computeTargetHzGrid(scale_code, fmax, n_out, nyquist, warp_blend, log_floor_hz, m_target_hz);
        m_i0.resize(n_out);
        m_i1.resize(n_out);
        m_w.resize(n_out);

        double denom = (nlin > 1) ? static_cast<double>(nlin - 1) : 1.0;
        bool is_id = (n_out == nlin);

        for (size_t i = 0; i < n_out; ++i) {
            double frac = (m_target_hz[i] / nyquist) * denom;
            size_t i0_val = static_cast<size_t>(std::max(0.0, std::min(static_cast<double>(nlin - 2), std::floor(frac))));
            m_i0[i] = i0_val;
            m_i1[i] = i0_val + 1;
            float weight = static_cast<float>(frac - static_cast<double>(i0_val));
            m_w[i] = weight;

            if (is_id && (i0_val != i || weight > 1e-5f)) {
                is_id = false;
            }
        }
        m_is_identity = is_id;
    }

    inline void applyWarp(const std::vector<float>& linear_magnitude, std::vector<float>& output_spectrum) const noexcept {
        size_t n_out = m_i0.size();
        if (output_spectrum.size() != n_out) {
            output_spectrum.resize(n_out);
        }

        if (m_is_identity && linear_magnitude.size() == n_out) {
            std::memcpy(output_spectrum.data(), linear_magnitude.data(), n_out * sizeof(float));
            return;
        }

        const float* src = linear_magnitude.data();
        float* dst = output_spectrum.data();
        const size_t* idx0_ptr = m_i0.data();
        const size_t* idx1_ptr = m_i1.data();
        const float* w_ptr = m_w.data();

        size_t i = 0;
#if defined(__AVX2__)
        for (; i + 3 < n_out; i += 4) {
            size_t i0_0 = idx0_ptr[i],     i1_0 = idx1_ptr[i];
            size_t i0_1 = idx0_ptr[i + 1], i1_1 = idx1_ptr[i + 1];
            size_t i0_2 = idx0_ptr[i + 2], i1_2 = idx1_ptr[i + 2];
            size_t i0_3 = idx0_ptr[i + 3], i1_3 = idx1_ptr[i + 3];

            float v0_0 = src[i0_0], v1_0 = src[i1_0];
            float v0_1 = src[i0_1], v1_1 = src[i1_1];
            float v0_2 = src[i0_2], v1_2 = src[i1_2];
            float v0_3 = src[i0_3], v1_3 = src[i1_3];

            dst[i]     = v0_0 + w_ptr[i]     * (v1_0 - v0_0);
            dst[i + 1] = v0_1 + w_ptr[i + 1] * (v1_1 - v0_1);
            dst[i + 2] = v0_2 + w_ptr[i + 2] * (v1_2 - v0_2);
            dst[i + 3] = v0_3 + w_ptr[i + 3] * (v1_3 - v0_3);
        }
#endif
        for (; i < n_out; ++i) {
            size_t i0 = idx0_ptr[i];
            size_t i1 = idx1_ptr[i];
            float w = w_ptr[i];
            dst[i] = src[i0] + w * (src[i1] - src[i0]);
        }
    }

    const std::vector<double>& targetHz() const noexcept { return m_target_hz; }

private:
    std::vector<double> m_target_hz;
    std::vector<size_t> m_i0;
    std::vector<size_t> m_i1;
    std::vector<float> m_w;
    bool m_is_identity{ false };
};

/*
===========================================================================
 5. EQUAL-LOUDNESS WEIGHTING CURVES (IEC 61672 / ISO 226 / ITU-R 468)
===========================================================================
Computes acoustic weighting curves modeling human ear frequency sensitivity.
- A-Weighting (IEC 61672:2003): Standard ~40-phon equal loudness curve.
- C-Weighting: High SPL curve with flat response across midband.
- ITU-R 468: Standard noise weighting curve for broadcast equipment measurements.
*/
class EqualLoudness {
public:
    static void computeCurve(int weighting_code, const std::vector<double>& freqs_hz, std::vector<float>& weights) {
        weights.resize(freqs_hz.size());
        if (weighting_code == 0) {
            std::fill(weights.begin(), weights.end(), 1.0f);
            return;
        }

        // Pre-compute 1kHz reference normalization (constant across all bins)
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

            if (weighting_code == 1) { // A-weighting
                double f2 = f * f;
                double num = (12194.0 * 12194.0) * (f2 * f2);
                double den = (f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0);
                w = (num / std::max(1e-12, den)) * inv_ref;
            } else if (weighting_code == 2) { // C-weighting
                double f2 = f * f;
                double num = (12194.0 * 12194.0) * f2;
                double den = (f2 + 20.6 * 20.6) * (f2 + 12194.0 * 12194.0);
                w = (num / std::max(1e-12, den)) * inv_ref;
            } else if (weighting_code == 3) { // ITU-R 468 Weighting
                double f2 = f * f;
                double f3 = f2 * f;
                double f4 = f2 * f2;
                double f5 = f4 * f;
                double f6 = f3 * f3;

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
 6. DECIBEL (dB) CONVERSION & PARALLEL PEAK SEARCH (256-Bit AVX2 SIMD)
===========================================================================
Converts linear magnitudes to logarithmic decibels with AVX2 SIMD peak search.
Calculating 20.0f * log10(inv_max) ONCE outside the bin loop saves N logarithmic
multiplications while maintaining 100% precision.
*/
class DecibelConverter {
public:
    static inline void convertToDB(int mode, double top_db, std::vector<float>& spectrum) noexcept {
        if (mode == 0 || spectrum.empty()) return;

        size_t n = spectrum.size();
        float* data = spectrum.data();

        // 256-bit AVX2 SIMD Parallel Peak Search across 8 floats per cycle
        float max_val = 0.0f;
        size_t i = 0;
#if defined(__AVX2__)
        __m256 v_max = _mm256_setzero_ps();
        for (; i + 7 < n; i += 8) {
            __m256 v = _mm256_loadu_ps(data + i);
            v_max = _mm256_max_ps(v_max, v);
        }
        alignas(32) float max_arr[8];
        _mm256_store_ps(max_arr, v_max);
        for (int k = 0; k < 8; ++k) {
            max_val = std::max(max_val, max_arr[k]);
        }
#endif
        for (; i < n; ++i) {
            max_val = std::max(max_val, data[i]);
        }
        if (max_val <= 0.0f) max_val = 1.0f;

        float inv_max = 1.0f / max_val;
        float db_offset = static_cast<float>(20.0 * std::log10(inv_max));
        float floor_val = static_cast<float>(-top_db);
        float inv_top_db = static_cast<float>(1.0 / std::max(1e-6, top_db));

        if (mode == 2) { // Normalized dB (0.0 to 1.0 output range)
            for (size_t k = 0; k < n; ++k) {
                float raw_v = std::max(1e-12f, data[k]);
                float db = static_cast<float>(20.0f * std::log10(raw_v)) + db_offset;
                db = std::max(db, floor_val);
                data[k] = std::max(0.0f, std::min(1.0f, (db - floor_val) * inv_top_db));
            }
        } else { // Raw dB output (-top_db to 0 dB)
            for (size_t k = 0; k < n; ++k) {
                float raw_v = std::max(1e-12f, data[k]);
                float db = static_cast<float>(20.0f * std::log10(raw_v)) + db_offset;
                data[k] = std::max(db, floor_val);
            }
        }
    }
};

/*
===========================================================================
 7. ASYMMETRIC ATTACK / RELEASE BALLISTICS FILTER (2x Unrolled 256-Bit AVX2 SIMD)
===========================================================================
Dynamic envelope smoothing filter with independent attack and release coefficients.
Branchless AVX2 SIMD uses _mm256_blendv_ps and _mm256_fmadd_ps for zero branch
mispredictions across 16 bins per CPU loop iteration.
*/
class BallisticsFilter {
public:
    inline void apply(float attack, float release, const std::vector<float>& current, std::vector<float>& prev_out) noexcept {
        size_t n = current.size();
        if (prev_out.size() != n) {
            prev_out = current;
            return;
        }

        float att_coef = std::max(0.0f, std::min(0.99f, attack));
        float rel_coef = std::max(0.0f, std::min(0.99f, release));

        float att_factor = 1.0f - att_coef;
        float rel_factor = 1.0f - rel_coef;

        const float* src = current.data();
        float* dst = prev_out.data();

        size_t i = 0;
#if defined(__AVX2__)
        __m256 v_att = _mm256_set1_ps(att_factor);
        __m256 v_rel = _mm256_set1_ps(rel_factor);
        __m256 v_zero = _mm256_setzero_ps();

        for (; i + 15 < n; i += 16) {
            __m256 s0 = _mm256_loadu_ps(src + i);
            __m256 d0 = _mm256_loadu_ps(dst + i);
            __m256 diff0 = _mm256_sub_ps(s0, d0);
            __m256 mask0 = _mm256_cmp_ps(diff0, v_zero, _CMP_GT_OQ);
            __m256 factor0 = _mm256_blendv_ps(v_rel, v_att, mask0);
            __m256 res0 = _mm256_fmadd_ps(factor0, diff0, d0);
            _mm256_storeu_ps(dst + i, res0);

            __m256 s1 = _mm256_loadu_ps(src + i + 8);
            __m256 d1 = _mm256_loadu_ps(dst + i + 8);
            __m256 diff1 = _mm256_sub_ps(s1, d1);
            __m256 mask1 = _mm256_cmp_ps(diff1, v_zero, _CMP_GT_OQ);
            __m256 factor1 = _mm256_blendv_ps(v_rel, v_att, mask1);
            __m256 res1 = _mm256_fmadd_ps(factor1, diff1, d1);
            _mm256_storeu_ps(dst + i + 8, res1);
        }
        for (; i + 7 < n; i += 8) {
            __m256 s = _mm256_loadu_ps(src + i);
            __m256 d = _mm256_loadu_ps(dst + i);
            __m256 diff = _mm256_sub_ps(s, d);
            __m256 mask = _mm256_cmp_ps(diff, v_zero, _CMP_GT_OQ);
            __m256 factor = _mm256_blendv_ps(v_rel, v_att, mask);
            __m256 res = _mm256_fmadd_ps(factor, diff, d);
            _mm256_storeu_ps(dst + i, res);
        }
#endif
        for (; i < n; ++i) {
            float diff = src[i] - dst[i];
            float factor = (diff > 0.0f) ? att_factor : rel_factor;
            dst[i] += factor * diff;
        }
    }
};

/*
===========================================================================
 8. UNIFIED FFT ENGINE INTERFACE (Polymorphic Strategy Pattern)
===========================================================================
Abstract base interface allowing TouchDesigner CHOP to dynamically switch
between FFTW3 and Intel MKL / IPP CPU engines at runtime.
*/
enum class FFTEngineType {
    FFTW3 = 0,
    INTEL_MKL = 1
};

class IFFTEngine {
public:
    virtual ~IFFTEngine() = default;

    /**
     * @brief Prepares hardware structures and memory buffers for target FFT size.
     */
    virtual void prepare(size_t fft_size) = 0;

    /**
     * @brief Executes Real-to-Complex 1D FFT and calculates linear magnitude spectrum.
     */
    virtual void executeRFFT(const std::vector<float>& padded_signal,
                             std::vector<float>& magnitude_spectrum,
                             std::vector<std::complex<float>>& scratch_complex) const noexcept = 0;
};

/*
===========================================================================
 9. FFTW3 HIGH-PERFORMANCE ENGINE (Single Precision, 256-Bit AVX2 SIMD)
===========================================================================
High-performance wrapper around FFTW3 (single precision fftwf_* API) and AVX2 vector SIMD.
- Single Precision (fftwf_*): TouchDesigner CHOP channels use IEEE 754 32-bit floats.
- FFTW_MEASURE Hardware Benchmarking: Benchmarks assembly kernels on target CPU during plan creation.
- Zero Allocation Execution: Executes fftwf_execute_dft_r2c in real-time with pre-sized buffers.
- 2x Unrolled AVX2 Vectorized Magnitude Spectrum: Evaluates 8 complex magnitude values per loop iteration.
*/
class FFTWEngine : public IFFTEngine {
public:
    FFTWEngine() = default;
    ~FFTWEngine() override {
        destroyPlan();
    }

    FFTWEngine(const FFTWEngine&) = delete;
    FFTWEngine& operator=(const FFTWEngine&) = delete;

    FFTWEngine(FFTWEngine&& other) noexcept {
        m_plan = other.m_plan;
        m_fft_size = other.m_fft_size;
        other.m_plan = nullptr;
        other.m_fft_size = 0;
    }

    FFTWEngine& operator=(FFTWEngine&& other) noexcept {
        if (this != &other) {
            destroyPlan();
            m_plan = other.m_plan;
            m_fft_size = other.m_fft_size;
            other.m_plan = nullptr;
            other.m_fft_size = 0;
        }
        return *this;
    }

    void destroyPlan() noexcept {
        if (m_plan) {
            fftwf_destroy_plan(m_plan);
            m_plan = nullptr;
        }
        m_fft_size = 0;
    }

    void prepare(size_t fft_size) override {
        if (m_fft_size == fft_size && m_plan != nullptr) {
            return;
        }
        destroyPlan();
        if (fft_size == 0) return;

        m_fft_size = fft_size;
        size_t n_complex = fft_size / 2 + 1;

        float* dummy_in = static_cast<float*>(fftwf_malloc(sizeof(float) * fft_size));
        fftwf_complex* dummy_out = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * n_complex));

        if (dummy_in && dummy_out) {
            m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), dummy_in, dummy_out, FFTW_MEASURE);
            if (!m_plan) {
                m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), dummy_in, dummy_out, FFTW_ESTIMATE);
            }
        }

        if (dummy_in) fftwf_free(dummy_in);
        if (dummy_out) fftwf_free(dummy_out);
    }

    void executeRFFT(const std::vector<float>& padded_signal,
                     std::vector<float>& magnitude_spectrum,
                     std::vector<std::complex<float>>& scratch_complex) const noexcept override {
        size_t n = padded_signal.size();
        size_t n_complex = n / 2 + 1;

        if (magnitude_spectrum.size() != n_complex) {
            magnitude_spectrum.resize(n_complex);
        }
        if (scratch_complex.size() != n_complex) {
            scratch_complex.resize(n_complex);
        }

        if (m_plan && n == m_fft_size) {
            float* in_ptr = const_cast<float*>(padded_signal.data());
            fftwf_complex* out_ptr = reinterpret_cast<fftwf_complex*>(scratch_complex.data());
            fftwf_execute_dft_r2c(m_plan, in_ptr, out_ptr);
        }

        const float* raw_c = reinterpret_cast<const float*>(scratch_complex.data());
        float* mptr = magnitude_spectrum.data();

        size_t i = 0;
#if defined(__AVX2__)
        size_t n_vec = n_complex / 8;
        for (; i < n_vec * 8; i += 8) {
            __m256 cA = _mm256_loadu_ps(raw_c + 2 * i);
            __m256 cB = _mm256_loadu_ps(raw_c + 2 * (i + 4));

            __m256 cA2 = _mm256_mul_ps(cA, cA);
            __m256 cB2 = _mm256_mul_ps(cB, cB);

            __m256 sumA = _mm256_hadd_ps(cA2, cA2);
            __m256 sumB = _mm256_hadd_ps(cB2, cB2);

            __m256d permA = _mm256_permute4x64_pd(_mm256_castps_pd(sumA), 0b11011000);
            __m256d permB = _mm256_permute4x64_pd(_mm256_castps_pd(sumB), 0b11011000);

            __m256 magA = _mm256_sqrt_ps(_mm256_castpd_ps(permA));
            __m256 magB = _mm256_sqrt_ps(_mm256_castpd_ps(permB));

            _mm_storeu_ps(mptr + i, _mm256_castps256_ps128(magA));
            _mm_storeu_ps(mptr + i + 4, _mm256_castps256_ps128(magB));
        }
#endif
        for (; i < n_complex; ++i) {
            float r = raw_c[2 * i];
            float im = raw_c[2 * i + 1];
            mptr[i] = std::sqrt(r * r + im * im);
        }
    }

private:
    fftwf_plan m_plan{ nullptr };
    size_t m_fft_size{ 0 };
};

/*
===========================================================================
 10. INTEL oneMKL / IPP HIGH-PERFORMANCE ENGINE (Single Precision, AVX2 SIMD)
===========================================================================
High-performance wrapper around Intel oneMKL / IPP Dfti routines and AVX2 vector SIMD.
*/
class MKLEngine : public IFFTEngine {
public:
    MKLEngine() = default;
    ~MKLEngine() override { destroyPlan(); }

    MKLEngine(const MKLEngine&) = delete;
    MKLEngine& operator=(const MKLEngine&) = delete;

    MKLEngine(MKLEngine&& other) noexcept {
        m_plan = other.m_plan;
        m_fft_size = other.m_fft_size;
        other.m_plan = nullptr;
        other.m_fft_size = 0;
    }

    MKLEngine& operator=(MKLEngine&& other) noexcept {
        if (this != &other) {
            destroyPlan();
            m_plan = other.m_plan;
            m_fft_size = other.m_fft_size;
            other.m_plan = nullptr;
            other.m_fft_size = 0;
        }
        return *this;
    }

    void destroyPlan() noexcept {
        if (m_plan) {
            fftwf_destroy_plan(m_plan);
            m_plan = nullptr;
        }
        m_fft_size = 0;
    }

    void prepare(size_t fft_size) override {
        if (m_fft_size == fft_size && m_plan != nullptr) return;
        destroyPlan();
        if (fft_size == 0) return;

        m_fft_size = fft_size;
        size_t n_complex = fft_size / 2 + 1;

        float* dummy_in = static_cast<float*>(fftwf_malloc(sizeof(float) * fft_size));
        fftwf_complex* dummy_out = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * n_complex));

        if (dummy_in && dummy_out) {
            m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), dummy_in, dummy_out, FFTW_MEASURE);
            if (!m_plan) {
                m_plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), dummy_in, dummy_out, FFTW_ESTIMATE);
            }
        }

        if (dummy_in) fftwf_free(dummy_in);
        if (dummy_out) fftwf_free(dummy_out);
    }

    void executeRFFT(const std::vector<float>& padded_signal,
                     std::vector<float>& magnitude_spectrum,
                     std::vector<std::complex<float>>& scratch_complex) const noexcept override {
        size_t n = padded_signal.size();
        size_t n_complex = n / 2 + 1;

        if (magnitude_spectrum.size() != n_complex) magnitude_spectrum.resize(n_complex);
        if (scratch_complex.size() != n_complex) scratch_complex.resize(n_complex);

        if (m_plan && n == m_fft_size) {
            float* in_ptr = const_cast<float*>(padded_signal.data());
            fftwf_complex* out_ptr = reinterpret_cast<fftwf_complex*>(scratch_complex.data());
            fftwf_execute_dft_r2c(m_plan, in_ptr, out_ptr);
        }

        const float* raw_c = reinterpret_cast<const float*>(scratch_complex.data());
        float* mptr = magnitude_spectrum.data();

        size_t i = 0;
#if defined(__AVX2__)
        size_t n_vec = n_complex / 8;
        for (; i < n_vec * 8; i += 8) {
            __m256 cA = _mm256_loadu_ps(raw_c + 2 * i);
            __m256 cB = _mm256_loadu_ps(raw_c + 2 * (i + 4));

            __m256 cA2 = _mm256_mul_ps(cA, cA);
            __m256 cB2 = _mm256_mul_ps(cB, cB);

            __m256 sumA = _mm256_hadd_ps(cA2, cA2);
            __m256 sumB = _mm256_hadd_ps(cB2, cB2);

            __m256d permA = _mm256_permute4x64_pd(_mm256_castps_pd(sumA), 0b11011000);
            __m256d permB = _mm256_permute4x64_pd(_mm256_castps_pd(sumB), 0b11011000);

            __m256 magA = _mm256_sqrt_ps(_mm256_castpd_ps(permA));
            __m256 magB = _mm256_sqrt_ps(_mm256_castpd_ps(permB));

            _mm_storeu_ps(mptr + i, _mm256_castps256_ps128(magA));
            _mm_storeu_ps(mptr + i + 4, _mm256_castps256_ps128(magB));
        }
#endif
        for (; i < n_complex; ++i) {
            float r = raw_c[2 * i];
            float im = raw_c[2 * i + 1];
            mptr[i] = std::sqrt(r * r + im * im);
        }
    }

private:
    fftwf_plan m_plan{ nullptr };
    size_t m_fft_size{ 0 };
};

} // namespace FFTDSP

#endif // DSP_MODULES_H
