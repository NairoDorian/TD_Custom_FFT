#ifndef CUFFT_DX_ENGINE_H
#define CUFFT_DX_ENGINE_H

/*
===========================================================================
               NVIDIA cuFFTDx FUSED KERNEL GPU ENGINE
===========================================================================
Header File: CUFFTDxEngine.h

Functional Overview:
Advanced GPU engine demonstrating cuFFTDx device-side fused kernel execution.
===========================================================================
*/

#include "IFFTEngine.h"
#include <cuda_runtime.h>
#include <memory>

namespace FFTDSP {

class CUFFTDxEngine : public IFFTEngine {
public:
    CUFFTDxEngine();
    ~CUFFTDxEngine() override;

    CUFFTDxEngine(const CUFFTDxEngine&) = delete;
    CUFFTDxEngine& operator=(const CUFFTDxEngine&) = delete;

    void prepare(size_t fft_size) override;

    void executeRFFT(const std::vector<float>& padded_signal,
                     std::vector<float>& magnitude_spectrum,
                     std::vector<std::complex<float>>& scratch_complex) const noexcept override;

private:
    void destroyPlan() noexcept;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace FFTDSP

#endif // CUFFT_DX_ENGINE_H
