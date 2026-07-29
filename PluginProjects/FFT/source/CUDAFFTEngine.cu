/*
===========================================================================
               NVIDIA cuFFT GPU ENGINE IMPLEMENTATION
===========================================================================
Source File: CUDAFFTEngine.cu

Implements 1D R2C FFT execution on NVIDIA GPU using cuFFT & CUDA kernels.
===========================================================================
*/

#include "CUDAFFTEngine.h"

namespace FFTDSP {

// CUDA Kernel: Parallel Complex Magnitude Spectrum Computation
__global__ void computeMagnitudeKernel(const cufftComplex* __restrict__ complex_in,
                                        float* __restrict__ mag_out,
                                        size_t num_bins) {
    size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < num_bins) {
        cufftComplex c = complex_in[idx];
        mag_out[idx] = sqrtf(c.x * c.x + c.y * c.y);
    }
}

CUDAFFTEngine::CUDAFFTEngine() {
    cudaStreamCreate(&m_stream);
}

CUDAFFTEngine::~CUDAFFTEngine() {
    destroyPlan();
    if (m_stream) {
        cudaStreamDestroy(m_stream);
        m_stream = nullptr;
    }
}

void CUDAFFTEngine::destroyPlan() noexcept {
    if (m_plan) {
        cufftDestroy(m_plan);
        m_plan = 0;
    }
    if (m_d_input) {
        cudaFree(m_d_input);
        m_d_input = nullptr;
    }
    if (m_d_output_complex) {
        cudaFree(m_d_output_complex);
        m_d_output_complex = nullptr;
    }
    if (m_d_magnitude) {
        cudaFree(m_d_magnitude);
        m_d_magnitude = nullptr;
    }
    m_fft_size = 0;
}

void CUDAFFTEngine::prepare(size_t fft_size) {
    if (m_fft_size == fft_size && m_plan != 0) {
        return;
    }
    destroyPlan();
    if (fft_size == 0) return;

    m_fft_size = fft_size;
    size_t n_complex = fft_size / 2 + 1;

    cudaMalloc(&m_d_input, sizeof(float) * fft_size);
    cudaMalloc(&m_d_output_complex, sizeof(cufftComplex) * n_complex);
    cudaMalloc(&m_d_magnitude, sizeof(float) * n_complex);

    if (cufftPlan1d(&m_plan, static_cast<int>(fft_size), CUFFT_R2C, 1) == CUFFT_SUCCESS) {
        cufftSetStream(m_plan, m_stream);
    }
}

void CUDAFFTEngine::executeRFFT(const std::vector<float>& padded_signal,
                                std::vector<float>& magnitude_spectrum,
                                std::vector<std::complex<float>>& scratch_complex) const noexcept {
    size_t n = padded_signal.size();
    size_t n_complex = n / 2 + 1;

    if (magnitude_spectrum.size() != n_complex) {
        magnitude_spectrum.resize(n_complex);
    }
    if (scratch_complex.size() != n_complex) {
        scratch_complex.resize(n_complex);
    }

    if (!m_plan || n != m_fft_size || !m_d_input || !m_d_output_complex || !m_d_magnitude) {
        return;
    }

    // 1. Copy host signal to GPU VRAM asynchronously
    cudaMemcpyAsync(m_d_input, padded_signal.data(), sizeof(float) * n, cudaMemcpyHostToDevice, m_stream);

    // 2. Execute R2C cuFFT on GPU
    cufftExecR2C(m_plan, m_d_input, m_d_output_complex);

    // 3. Launch parallel GPU kernel for magnitude calculation
    int threads_per_block = 256;
    int blocks = static_cast<int>((n_complex + threads_per_block - 1) / threads_per_block);
    computeMagnitudeKernel<<<blocks, threads_per_block, 0, m_stream>>>(m_d_output_complex, m_d_magnitude, n_complex);

    // 4. Copy computed magnitude spectrum back to host RAM asynchronously & sync stream
    cudaMemcpyAsync(magnitude_spectrum.data(), m_d_magnitude, sizeof(float) * n_complex, cudaMemcpyDeviceToHost, m_stream);
    cudaMemcpyAsync(scratch_complex.data(), m_d_output_complex, sizeof(cufftComplex) * n_complex, cudaMemcpyDeviceToHost, m_stream);

    cudaStreamSynchronize(m_stream);
}

} // namespace FFTDSP
