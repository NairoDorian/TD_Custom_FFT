#ifndef IFFT_ENGINE_H
#define IFFT_ENGINE_H

/*
===========================================================================
                     UNIFIED FFT ENGINE INTERFACE
===========================================================================
Header File: IFFTEngine.h

Architectural Design Pattern: Polymorphic Strategy Pattern
Abstract base interface allowing TouchDesigner CHOP to dynamically switch
between CPU (FFTW3) and GPU (cuFFT, VkFFT, cuFFTDx) backends at runtime.
===========================================================================
*/

#include <vector>
#include <complex>
#include <cstddef>

namespace FFTDSP {

enum class FFTEngineType {
    CPU_FFTW3 = 0,
    CPU_INTEL_MKL = 1,
    GPU_CUFFT = 2,
    GPU_VKFFT = 3,
    GPU_CUFFTDX = 4
};

class IFFTEngine {
public:
    virtual ~IFFTEngine() = default;

    /**
     * @brief Prepares hardware structures (FFTW plans, CUDA streams, VRAM allocations) for target FFT size.
     */
    virtual void prepare(size_t fft_size) = 0;

    /**
     * @brief Executes Real-to-Complex 1D FFT and calculates linear magnitude spectrum.
     */
    virtual void executeRFFT(const std::vector<float>& padded_signal,
                           std::vector<float>& magnitude_spectrum,
                           std::vector<std::complex<float>>& scratch_complex) const noexcept = 0;
};

} // namespace FFTDSP

#endif // IFFT_ENGINE_H
