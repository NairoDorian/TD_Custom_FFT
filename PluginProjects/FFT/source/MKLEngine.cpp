/*
===========================================================================
               INTEL oneMKL / IPP CPU ENGINE IMPLEMENTATION
===========================================================================
Source File: MKLEngine.cpp

Implements Intel oneMKL / IPP 1D Real-to-Complex FFT execution.
===========================================================================
*/

#include "MKLEngine.h"
#include <fftw3.h>
#include <cmath>
#include <immintrin.h>

namespace FFTDSP {

struct MKLEngine::Impl {
    fftwf_plan plan{ nullptr };
    size_t fft_size{ 0 };

    Impl() = default;
    ~Impl() { destroy(); }

    void destroy() {
        if (plan) {
            fftwf_destroy_plan(plan);
            plan = nullptr;
        }
        fft_size = 0;
    }
};

MKLEngine::MKLEngine() : m_impl(std::make_unique<Impl>()) {}

MKLEngine::~MKLEngine() = default;

void MKLEngine::destroyPlan() noexcept {
    if (m_impl) m_impl->destroy();
}

void MKLEngine::prepare(size_t fft_size) {
    if (!m_impl) return;
    if (m_impl->fft_size == fft_size && m_impl->plan != nullptr) return;

    m_impl->destroy();
    if (fft_size == 0) return;

    m_impl->fft_size = fft_size;
    size_t n_complex = fft_size / 2 + 1;

    float* dummy_in = static_cast<float*>(fftwf_malloc(sizeof(float) * fft_size));
    fftwf_complex* dummy_out = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * n_complex));

    if (dummy_in && dummy_out) {
        m_impl->plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), dummy_in, dummy_out, FFTW_MEASURE);
        if (!m_impl->plan) {
            m_impl->plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fft_size), dummy_in, dummy_out, FFTW_ESTIMATE);
        }
    }

    if (dummy_in) fftwf_free(dummy_in);
    if (dummy_out) fftwf_free(dummy_out);
}

void MKLEngine::executeRFFT(const std::vector<float>& padded_signal,
                            std::vector<float>& magnitude_spectrum,
                            std::vector<std::complex<float>>& scratch_complex) const noexcept {
    if (!m_impl) return;

    size_t n = padded_signal.size();
    size_t n_complex = n / 2 + 1;

    if (magnitude_spectrum.size() != n_complex) magnitude_spectrum.resize(n_complex);
    if (scratch_complex.size() != n_complex) scratch_complex.resize(n_complex);

    if (!m_impl->plan || n != m_impl->fft_size) return;

    // 1. Execute Intel MKL / optimized R2C DFT
    float* in_ptr = const_cast<float*>(padded_signal.data());
    fftwf_complex* out_ptr = reinterpret_cast<fftwf_complex*>(scratch_complex.data());
    fftwf_execute_dft_r2c(m_impl->plan, in_ptr, out_ptr);

    // 2. 256-Bit AVX2 SIMD Magnitude Spectrum Computation
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

} // namespace FFTDSP
