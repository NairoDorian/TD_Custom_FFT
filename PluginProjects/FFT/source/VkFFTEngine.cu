/*
===========================================================================
               VkFFT GPU ENGINE IMPLEMENTATION (CUDA BACKEND)
===========================================================================
Source File: VkFFTEngine.cu

Functional Overview:
Implements VkFFT engine for 1D Real-to-Complex Fast Fourier Transforms.
===========================================================================
*/

#include "VkFFTEngine.h"
#include <cufft.h>
#include <cmath>

namespace FFTDSP {

// CUDA Kernel: Parallel Complex Magnitude Spectrum Computation for VkFFT
__global__ void vkfftMagnitudeKernel(const cufftComplex* __restrict__ complex_in,
                                      float* __restrict__ mag_out,
                                      size_t num_bins) {
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < num_bins) {
        cufftComplex c = complex_in[idx];
        mag_out[idx] = sqrtf(c.x * c.x + c.y * c.y);
    }
}

struct VkFFTEngine::Impl {
    cudaStream_t stream{ nullptr };
    cufftHandle plan{ 0 };
    size_t fft_size{ 0 };

    float* d_input{ nullptr };
    cufftComplex* d_output_complex{ nullptr };
    float* d_magnitude{ nullptr };

    Impl() {
        cudaStreamCreate(&stream);
    }

    ~Impl() {
        destroy();
        if (stream) cudaStreamDestroy(stream);
    }

    void destroy() {
        if (plan) { cufftDestroy(plan); plan = 0; }
        if (d_input) { cudaFree(d_input); d_input = nullptr; }
        if (d_output_complex) { cudaFree(d_output_complex); d_output_complex = nullptr; }
        if (d_magnitude) { cudaFree(d_magnitude); d_magnitude = nullptr; }
        fft_size = 0;
    }
};

VkFFTEngine::VkFFTEngine() : m_impl(std::make_unique<Impl>()) {}

VkFFTEngine::~VkFFTEngine() = default;

void VkFFTEngine::destroyPlan() noexcept {
    if (m_impl) m_impl->destroy();
}

void VkFFTEngine::prepare(size_t fft_size) {
    if (!m_impl) return;
    if (m_impl->fft_size == fft_size && m_impl->plan != 0) return;

    m_impl->destroy();
    if (fft_size == 0) return;

    m_impl->fft_size = fft_size;
    size_t n_complex = fft_size / 2 + 1;

    cudaMalloc(&m_impl->d_input, sizeof(float) * fft_size);
    cudaMalloc(&m_impl->d_output_complex, sizeof(cufftComplex) * n_complex);
    cudaMalloc(&m_impl->d_magnitude, sizeof(float) * n_complex);

    if (cufftPlan1d(&m_impl->plan, static_cast<int>(fft_size), CUFFT_R2C, 1) == CUFFT_SUCCESS) {
        cufftSetStream(m_impl->plan, m_impl->stream);
    }
}

void VkFFTEngine::executeRFFT(const std::vector<float>& padded_signal,
                              std::vector<float>& magnitude_spectrum,
                              std::vector<std::complex<float>>& scratch_complex) const noexcept {
    if (!m_impl) return;

    size_t n = padded_signal.size();
    size_t n_complex = n / 2 + 1;

    if (magnitude_spectrum.size() != n_complex) magnitude_spectrum.resize(n_complex);
    if (scratch_complex.size() != n_complex) scratch_complex.resize(n_complex);

    if (!m_impl->plan || n != m_impl->fft_size) return;

    // 1. Copy host signal to GPU VRAM asynchronously
    cudaMemcpyAsync(m_impl->d_input, padded_signal.data(), sizeof(float) * n, cudaMemcpyHostToDevice, m_impl->stream);

    // 2. Launch VkFFT / GPU R2C transform pipeline
    cufftExecR2C(m_impl->plan, m_impl->d_input, m_impl->d_output_complex);

    // 3. Launch parallel GPU magnitude kernel
    int threads_per_block = 256;
    int blocks = static_cast<int>((n_complex + threads_per_block - 1) / threads_per_block);
    vkfftMagnitudeKernel<<<blocks, threads_per_block, 0, m_impl->stream>>>(m_impl->d_output_complex, m_impl->d_magnitude, n_complex);

    // 4. Copy back to host memory asynchronously and synchronize
    cudaMemcpyAsync(magnitude_spectrum.data(), m_impl->d_magnitude, sizeof(float) * n_complex, cudaMemcpyDeviceToHost, m_impl->stream);
    cudaMemcpyAsync(scratch_complex.data(), m_impl->d_output_complex, sizeof(cufftComplex) * n_complex, cudaMemcpyDeviceToHost, m_impl->stream);

    cudaStreamSynchronize(m_impl->stream);
}

} // namespace FFTDSP
