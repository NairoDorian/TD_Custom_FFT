#ifndef MKL_ENGINE_H
#define MKL_ENGINE_H

/*
===========================================================================
               INTEL oneMKL / IPP HIGH-PERFORMANCE CPU ENGINE
===========================================================================
Header File: MKLEngine.h

Functional Overview:
CPU-accelerated Real-to-Complex 1D FFT implementation utilizing Intel oneMKL
(Math Kernel Library) / Intel IPP DFTI descriptors and AVX2 vector SIMD.
===========================================================================
*/

#include "IFFTEngine.h"
#include <memory>

namespace FFTDSP {

class MKLEngine : public IFFTEngine {
public:
    MKLEngine();
    ~MKLEngine() override;

    MKLEngine(const MKLEngine&) = delete;
    MKLEngine& operator=(const MKLEngine&) = delete;

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

#endif // MKL_ENGINE_H
