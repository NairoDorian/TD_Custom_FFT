#ifndef VK_FFT_ENGINE_H
#define VK_FFT_ENGINE_H

/*
===========================================================================
               VkFFT CROSS-PLATFORM GPU ENGINE (CUDA BACKEND)
===========================================================================
Header File: VkFFTEngine.h

Functional Overview:
High-performance cross-platform GPU FFT backend powered by VkFFT (VKFFT_BACKEND=1 CUDA).
===========================================================================
*/

#include "IFFTEngine.h"
#include <cuda_runtime.h>
#include <memory>

namespace FFTDSP {

class VkFFTEngine : public IFFTEngine {
public:
    VkFFTEngine();
    ~VkFFTEngine() override;

    VkFFTEngine(const VkFFTEngine&) = delete;
    VkFFTEngine& operator=(const VkFFTEngine&) = delete;

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

#endif // VK_FFT_ENGINE_H
