#ifndef IFFT_ENGINE_H
#define IFFT_ENGINE_H

/*
===========================================================================
                     UNIFIED FFT ENGINE INTERFACE
===========================================================================
Header File: IFFTEngine.h

Architectural Design Pattern: Polymorphic Strategy Pattern
Abstract base interface allowing TouchDesigner CHOP to dynamically switch
between FFTW3 and Intel MKL / IPP CPU engines at runtime.
===========================================================================
*/

#include <vector>
#include <complex>
#include <cstddef>

namespace FFTDSP {

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

} // namespace FFTDSP

#endif // IFFT_ENGINE_H
