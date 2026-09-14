// Shared Use License: This file is owned by Derivative Inc. (Derivative)
// and can only be used, and/or modified for use, in conjunction with
// Derivative's TouchDesigner software, and only if you are a licensee who has
// accepted Derivative's TouchDesigner license or assignment agreement
// (which also govern the use of this file). You may share or redistribute
// a modified version of this file provided the following conditions are met:
//
// 1. The shared file or redistribution must retain the information set out
//    above and use the Shared Usage License.
//
// 2. This file must be distributed with Derivative's software.
//
// 3. This file is distributed in the hope that it will be useful, with the
//    understanding that Derivative Inc. makes NO WARRANTIES regarding the
//    use of this file, and Derivative specifically disclaims all implied
//    warranties of merchantability
//
#pragma once

// ---------------------------------------------------------------------------------------------
// TD-free sample-rate / bin / axis model.
//
// TouchDesigner asks a CHOP for a single "sample rate", but for a spectrum a single
// number has to mean one of two different things, and this node reports one reading of
// each:
//
//  * sampleRateToTouchDesigner() — the CONCATENATED-STREAM reading: one output vector of
//    `bins` samples is emitted every cook, so the node emits `bins * cookRate` samples
//    per second (bins = me.time.rate). This is what TouchDesigner's downstream CHOPs
//    assume (an Audio Spectrum CHOP's bin-to-Hz math uses the sample rate), so it is what
//    info->sampleRate reports. It carries no bin-to-Hz information.
//
//  * axisRate() — the band the frequency axis covers, in the standard "bin 0 is DC, the
//    last bin is Nyquist" form: 2 * (top of the axis) = 2 * fmax. Display Max is clamped to
//    Nyquist, so this is 2 * min(Display Max, Nyquist) — the input rate when Display Max
//    reaches the top of the band. This does NOT depend on the bin count. This is what
//    hzPerBin() and the `output_spectrum_axis` Info row are derived from.
//
// Everything in here is a pure function of the parameters and the measured frame values
// (cook rate, cook delta, the axis rate the last pipeline published) — no OP_Inputs, no
// atomics. That is what makes the rate model unit-testable headlessly, and is where the
// v2.5.0/v2.6.0 sample-rate bugs lived (and can't recur once pinned).
// ---------------------------------------------------------------------------------------------

#include "Parameters.h"

#include <algorithm>
#include <cmath>

// Window length in samples for the given parameters and input rate (ms mode converts with the rate).
// Shared by the CHOP (getOutputInfo/execute) and the pipeline (rebuild) so they cannot disagree.
inline int windowSamplesFrom(const Parameters::Values& p, double sampleRate)
{
	if (p.winMode == Parameters::WinMode::Milliseconds) {
		return std::clamp(static_cast<int>(std::lround(p.winMs * sampleRate / 1000.0)), 1, Parameters::kMaxWinSamples);
	}
	return p.winSamples;
}

// FFT size >= the zero-pad choice and >= the next power of two of the window.
inline size_t fftSizeFrom(const Parameters::Values& p, int winSamples)
{
	size_t needed = 1;
	while (needed < static_cast<size_t>(std::max(1, winSamples))) needed *= 2;
	return std::max<size_t>(static_cast<size_t>(std::max(2, p.padSize)), needed);
}

// Output sample count. getOutputInfo and execute must agree on this — TouchDesigner allocates
// the output buffer from here — so it lives in one place, and updateWarp() builds its grid with
// the same number.
inline int outputBinCountFrom(const Parameters::Values& p)
{
	return p.bins;
}

// Axis rate (2 * top of axis). `axisRateExact` is the rate read off the tables the last pipeline
// published (0 when nothing has run yet); when present it is authoritative — the reported axis
// must never jump once the first result lands, and the live tables already encode any clamping
// that happened at build time (Display Max clamped to Nyquist).
inline double axisRate(const Parameters::Values& p, double sampleRate, double axisRateExact)
{
	const double nyquist = sampleRate * 0.5;
	if (nyquist <= 0.0) return sampleRate;
	if (axisRateExact > 0.0) return axisRateExact;
	// Not built yet. 2*fmax with fmax = min(Display Max, nyquist) — identical to what updateWarp
	// publishes, so the reported axis never jumps once the first result lands.
	const double fmax = std::min(p.displayMax > 0.0 ? p.displayMax : nyquist, nyquist);
	return 2.0 * fmax;
}

// Sample rate reported to TouchDesigner for the spectrum: bins * cookRate. Deliberately takes no
// input rate — passing it in is what used to claim the spectrum reaches sr_in/2 Hz when it stops
// at Display Max (the v2.5.0/2.6.0 mistake).
inline double sampleRateToTouchDesigner(const Parameters::Values& p, double cookRate)
{
	const int n_out = outputBinCountFrom(p);
	return static_cast<double>(n_out) * (cookRate > 0.0 ? cookRate : 60.0);
}

// Hz per output bin — the index-to-Hz mapping, exactly fmax/(bins-1): the bin spacing of the grid
// that was built. Uniform for a linear/identity grid, the mean spacing for a perceptual grid.
inline double hzPerBin(const Parameters::Values& p, double sampleRate, double axisRateExact)
{
	const int n_out = outputBinCountFrom(p);
	if (n_out < 2) return 0.0;
	return axisRate(p, sampleRate, axisRateExact) / (2.0 * static_cast<double>(n_out - 1));
}

// Data throughput in samples per second — sampleRateToTouchDesigner() but measured instead of
// declared (`bins` samples actually left the node over the cook delta actually observed). The two
// numbers differ only by the rate assumption; this one sanity-checks a buffer/ring/upload/send.
inline double throughput(const Parameters::Values& p, double cookDtMs)
{
	const int n_out = outputBinCountFrom(p);
	if (!(cookDtMs > 0.0)) return 0.0;
	return static_cast<double>(n_out) * 1000.0 / cookDtMs;
}
