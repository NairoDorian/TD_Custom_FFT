#ifndef CUDA_FFT_ENGINE_H
#define CUDA_FFT_ENGINE_H

/*
===========================================================================
               NVIDIA cuFFT HIGH-PERFORMANCE GPU ENGINE
===========================================================================
Header File: CUDAFFTEngine.h

Functional Overview:
GPU-accelerated Real-to-Complex 1D FFT implementation utilizing NVIDIA cuFFT
and async CUDA streams for high-throughput TouchDesigner CHOP processing.
===========================================================================
*/

#include "IFFTEngine.h"
#include <cufft.h>
#include <cuda_runtime.h>
#include <iostream>

namespace FFTDSP {

class CUDAFFTEngine : public IFFTEngine {
public:
    CUDAFFTEngine();
    ~CUDAFFTEngine() override;

    CUDAFFTEngine(const CUDAFFTEngine&) = delete;
    CUDAFFTEngine& operator=(const CUDAFFTEngine&) = delete;

    void prepare(size_t fft_size) override;

    void executeRFFT(const std::vector<float>& padded_signal,
                     std::vector<float>& magnitude_spectrum,
                     std::vector<std::complex<float>>& scratch_complex) const noexcept override;

private:
    void destroyPlan() noexcept;

    mutable cufftHandle m_plan{ 0 };
    mutable cudaStream_t m_stream{ nullptr };
    size_t m_fft_size{ 0 };

    mutable float* m_d_input{ nullptr };
    mutable cufftComplex* m_d_output_complex{ nullptr };
    mutable float* m_d_magnitude{ nullptr };
};

} // namespace FFTDSP

#endif // CUDA_FFT_ENGINE_H
