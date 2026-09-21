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

// ---------------------------------------------------------------------------------------------
// HOW TO READ THIS FILE (plain version of the two readings above)
//
// Every function here answers one question about size, rate or spacing, and each belongs to
// exactly ONE of the two "sample rate" readings the header describes. Getting them mixed up is the
// bug this file exists to prevent, so each function below says which reading it is.
//
// WHAT IT COMPUTES / WHY / HOW TO CHANGE is written per function. The shape they all share:
// they take a Parameters::Values snapshot plus the measured frame values, and return a number.
// They are pure: same inputs, same answer, no TouchDesigner state, no side effects.
//
// HOW TO CHANGE ANYTHING HERE: these values are the contract between three places that must agree -
// FFT::getOutputInfo() (which sizes the output buffer TouchDesigner allocates), FFT::execute()
// (which fills it) and AnalysisPipeline (which builds the grid that decides what each bin means).
// TouchDesigner allocates from the same number execute() writes, so if one of these functions is
// changed to disagree with its caller the result is a buffer overrun, not a wrong reading. Change
// the function, and the callers follow automatically; hard-code a number at a call site instead and
// the two can silently diverge.
// ---------------------------------------------------------------------------------------------

// Window length in samples for the given parameters and input rate (ms mode converts with the rate).
// Shared by the CHOP (getOutputInfo/execute) and the pipeline (rebuild) so they cannot disagree.
//
// WHAT: how many input samples one analysis window holds. In Samples mode this is the parameter
//       value passed straight through; in Milliseconds mode it is winMs converted to samples at the
//       input rate, rounded to nearest and clamped to at least 1.
// UNITS: samples (not seconds). It is a TIME-domain length: it is how much audio is tapered, and it
//       is the length of the FIFO that holds the history. It is NOT the FFT size - zero-padding
//       (see fftSizeFrom) makes the transform longer than the window.
// READING: belongs to neither of the two output "sample rates". It uses the INPUT sample rate
//       (the audio rate), which is the one number the two readings above are not about.
// WHY IT LIVES HERE: the CHOP computes the FIFO capacity from it (FFT::executeImpl(), the body
//       behind FFT::execute) and the pipeline sizes the same FIFO on rebuild; one function keeps
//       those two from disagreeing.
// HOW TO CHANGE: kMaxWinSamples is the hard cap in both modes. Raising it raises the largest FFT
//       that can be forced by the window (fftSizeFrom rounds up to the next power of two), which
//       costs planning time and memory; lowering it silently clips any project already set above it.
// CALLED BY: FFT::executeImpl() (the body behind FFT::execute) and, through the job's winSamples
//       field, AnalysisPipeline::rebuild(). Also exercised by test_rate_model() in
//       tests/dsp_tests.cpp.
inline int windowSamplesFrom(const Parameters::Values& p, double sampleRate)
{
	if (p.winMode == Parameters::WinMode::Milliseconds) {
		return std::clamp(static_cast<int>(std::lround(p.winMs * sampleRate / 1000.0)), 1, Parameters::kMaxWinSamples);
	}
	return p.winSamples;
}

// FFT size >= the zero-pad choice and >= the next power of two of the window.
//
// WHAT: how many points the FFT itself runs on. It is the larger of two things: the chosen zero-pad
//       length (p.padSize), and the window length rounded UP to the next power of two. The result is
//       always a power of two, because that is what the transform library wants.
// UNITS: samples of the transform (a count, not a rate).
// READING: neither output reading - it is a transform size. Do not confuse it with p.bins: the FFT
//       produces fftSize/2 + 1 real bins, and the node then resamples those onto p.bins output bins.
// WHY THE PAD FLOOR EXISTS: zero-padding a windowed block and centring it is what keeps a transient
//       away from the frame edges (see the centre-offset note in AnalysisPipeline::rebuild). The pad
//       menu therefore sets a minimum transform length, and a window longer than the pad wins.
// HOW TO CHANGE: it must never come back smaller than the window, or the window would be truncated
//       and the analysis would taper the wrong thing - that is the whole reason for the max().
//       padSize comes from the pad menu index in eval(); a bad index falls back to kPadDefault.
// CALLED BY: AnalysisPipeline::rebuild() (source/AnalysisPipeline.cpp) - the only caller in the
//       plugin. Also exercised by test_rate_model() in tests/dsp_tests.cpp. Not used by bench/.
inline size_t fftSizeFrom(const Parameters::Values& p, int winSamples)
{
	size_t needed = 1;
	while (needed < static_cast<size_t>(std::max(1, winSamples))) needed *= 2;
	return std::max<size_t>(static_cast<size_t>(std::max(2, p.padSize)), needed);
}

// Output sample count. getOutputInfo and execute must agree on this — TouchDesigner allocates
// the output buffer from here — so it lives in one place, and updateWarp() builds its grid with
// the same number.
//
// WHAT: how many samples one output channel of this CHOP has per cook - the number of spectrum
//       bins the user asked for, passed straight through from the Output Bins parameter.
// UNITS: samples (bins) per channel per cook. A count, not a rate.
// READING: this is not a sample rate, but it is the multiplier in BOTH readings: the reported
//       sample rate is this times the cook rate (sampleRateToTouchDesigner), and the reported
//       throughput is this times the measured frame rate (throughput).
// WHY IT IS A FUNCTION AND NOT A FIELD READ: getOutputInfo() declares the output size from it and
//       TouchDesigner allocates that size for execute() to fill, so the two must never disagree;
//       routing the declaration through one function makes that agreement structural rather than a
//       convention. FFT::execute() itself never calls it - the function that fills the buffer,
//       FFT::copyResultsToOutput() (reached from FFT::executeImpl()), works from output->numSamples,
//       the size TouchDesigner was given.
// HOW TO CHANGE: nothing else to change - but note this is the one place the output width is
//       defined, so a future "auto" bin count belongs here and nowhere else.
// CALLED BY: FFT::getOutputInfo(), which sets info->numSamples from it - the number the output
//       buffer is allocated at and the number FFT::execute() then fills - plus the Info CHOP/DAT
//       and popup strings in source/FFT.cpp, AnalysisPipeline::updateWarp() in
//       source/AnalysisPipeline.cpp, and the other functions in this file. Also exercised by
//       test_rate_model() in tests/dsp_tests.cpp.
inline int outputBinCountFrom(const Parameters::Values& p)
{
	return p.bins;
}

// Axis rate (2 * top of axis). `axisRateExact` is the rate read off the tables the last pipeline
// published (0 when nothing has run yet); when present it is authoritative — the reported axis
// must never jump once the first result lands, and the live tables already encode any clamping
// that happened at build time (Display Max clamped to Nyquist).
//
// WHAT: the width of the frequency band the output axis covers, in Hz, written in the form the
//       rest of the world expects: twice the top frequency of the band. If the highest frequency
//       on the axis is 24 kHz, this is 48000.
// UNITS: Hz. It is a frequency span, NOT samples per second - the name says "rate" only because it
//       is deliberately in the same units as a sample rate so it can be compared with one.
// READING: this is the AXIS reading, the second of the two in the header. It is what hzPerBin() is
//       derived from and what the `output_spectrum_axis` Info row reports. It is NOT what
//       info->sampleRate reports - that is sampleRateToTouchDesigner() below, the concatenated-
//       stream reading, and the two have nothing to do with each other except that they are both
//       written in Hz.
//
// HOW IT RELATES TO sampleRateToTouchDesigner(): they are independent quantities. The only exact
// relationship is a special case: this function returns exactly the INPUT sample rate when Display
// Max reaches (or passes) Nyquist, because then fmax = sampleRate/2 and 2 * fmax = sampleRate. Set
// Display Max below Nyquist (say 10 kHz at 48 kHz in) and this is 20000 while the input rate is
// still 48000; set it at/above Nyquist and the two coincide. The concatenated rate is bins * cook
// rate and has no such relation - at the defaults it is 983040 while this is 48000.
//
// WHY THE "2 *" FORM IS EXACT, NOT A CONVENTION: the grid is built so bin 0 sits on the bottom of
// the band and the LAST bin sits exactly on fmax (test_identity_grid_and_rate() in
// tests/dsp_tests.cpp states and checks this for every scale). "Bin 0 is DC, the last bin is
// Nyquist" is then the standard reading - the last bin is at half the axis rate, i.e. Nyquist -
// and the spacing works out to exactly fmax/(bins-1).
// Report the axis any other way and a consumer reading a bin index back to Hz lands somewhere else.
//
// HOW TO CHANGE: Display Max is clamped to Nyquist here and by updateWarp(), so raising Display Max
//       past Nyquist cannot claim a band the input does not contain. `axisRateExact` is preferred
//       over the recomputed value whenever the pipeline has published one, which is what stops the
//       reported number from jumping between "not built yet" and "built": if you change the
//       fallback formula here, change it in AnalysisPipeline::updateWarp() too or the number will
//       change the instant the first result lands.
// CALLED BY: FFT::outputAxisRate(), which is the only caller in FFT.cpp - the Info DAT axis row and
//       popup reach the number through it, and the Info CHOP reads the pipeline's published copy -
//       plus hzPerBin() below, and test_rate_model() in tests/dsp_tests.cpp. Not used by bench/.
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
//
// WHAT: the value the node hands TouchDesigner as info->sampleRate. It is the number of samples
//       the node emits per second if you lay one frame's spectrum end to end after another:
//       Output Bins x the cook rate. At the defaults (16384 bins, 60 fps) that is 983040.
// UNITS: samples per second (Hz in the "how fast is data arriving" sense).
// READING: this is the CONCATENATED-STREAM reading, the first of the two in the header, and it is
//       the one the CHOP's `sampleRate` field must carry because downstream CHOPs assume a time
//       reading. It says nothing about what any bin means in frequency - that is axisRate() /
//       hzPerBin().
// WHY IT TAKES NO INPUT RATE: passing the audio rate in here is the v2.5.0/2.6.0 bug named above.
//       The signature is the fix: there is no parameter to pass it through, so the mistake cannot
//       be reintroduced by a caller.
// HOW TO CHANGE: cookRate <= 0 falls back to 60 (a positive rate is assumed downstream). Note the
//       fallback is NOT the same as the <= 0 guard in throughput() - that one reports 0 because a
//       measurement of nothing should not invent a rate, while this one is a declaration and must
//       produce something usable. Do not unify them without deciding which behaviour the CHOP
//       contract needs.
// CALLED BY: FFT::outputSampleRate() - which is what info->sampleRate is set from in
//       FFT::getOutputInfo(), and what the Info CHOP/DAT and popup rows report - and
//       test_rate_model() in tests/dsp_tests.cpp. Not used by bench/.
inline double sampleRateToTouchDesigner(const Parameters::Values& p, double cookRate)
{
	const int n_out = outputBinCountFrom(p);
	return static_cast<double>(n_out) * (cookRate > 0.0 ? cookRate : 60.0);
}

// Hz per output bin — the index-to-Hz mapping, exactly fmax/(bins-1): the bin spacing of the grid
// that was built. Uniform for a linear/identity grid, the mean spacing for a perceptual grid.
//
// WHAT: how many Hz one step along the output bins is worth. TouchDesigner's Audio Spectrum CHOP
//       exposes the same idea as `hz_per_sample`; this is the node's equivalent, and it is the
//       number to multiply a bin index by when all you need is a rough frequency.
// UNITS: Hz per bin.
// READING: belongs to the AXIS reading (it is axisRate/2 spread over the bins). It is the ONLY
//       channel that carries the bin-index-to-Hz mapping: the reported sample rate does not.
// WHY IT IS "exactly fmax/(bins-1)": axisRate()/2 is fmax and there are (bins-1) intervals from
//       bin 0 to the last bin, because the last bin sits on fmax. That division is the whole
//       formula - it is not an approximation.
// WHY IT IS AN AVERAGE FOR A PERCEPTUAL GRID: only a uniform axis (Scale = Linear, or Warp Blend
//       = 0) has one spacing everywhere. On a Log/Mel/ERB/Bark/Chroma grid the spacing varies, and
//       the mean is the one honest single number; multiply a bin index by it and expect error away
//       from a linear axis. Nothing here is wrong - do not "fix" it by reporting the first spacing.
// HOW TO CHANGE: n_out < 2 returns 0 because a single bin has no spacing between anything (and the
//       division would be by zero). Keep that guard: callers divide the result back out.
// CALLED BY: FFT::hzPerSample() and test_rate_model() in tests/dsp_tests.cpp. Not used by bench/.
//       The Info CHOP/DAT and popup read it through FFT::hzPerSample().
inline double hzPerBin(const Parameters::Values& p, double sampleRate, double axisRateExact)
{
	const int n_out = outputBinCountFrom(p);
	if (n_out < 2) return 0.0;
	return axisRate(p, sampleRate, axisRateExact) / (2.0 * static_cast<double>(n_out - 1));
}

// Data throughput in samples per second — sampleRateToTouchDesigner() but measured instead of
// declared (`bins` samples actually left the node over the cook delta actually observed). The two
// numbers differ only by the rate assumption; this one sanity-checks a buffer/ring/upload/send.
//
// WHAT: the same quantity as sampleRateToTouchDesigner(), but worked out from the frame time that
//       actually elapsed rather than the nominal timeline rate: Output Bins x 1000 / frame-ms.
// UNITS: samples per second.
// READING: the CONCATENATED-STREAM reading, like sampleRateToTouchDesigner() - it is this same
//       physical quantity measured rather than declared. Both differ only in which frame duration
//       they divide by (the measured cook delta vs. me.time.rate). Neither is the axis reading.
// WHY BOTH EXIST: a declaration can be wrong (a node that is not keeping up still declares the
//       nominal rate). This one is what the data really did, so it is the figure to size or verify
//       a ring buffer, a GPU upload or a network send against. The Info DAT and the popup print the
//       two on one line (the `Rate:` row) for that comparison.
// HOW TO CHANGE: a non-positive or zero frame delta returns 0, not infinity - a zero-length frame
//       has no rate. That is deliberately less forgiving than sampleRateToTouchDesigner()'s 60 fps
//       fallback; do not make them consistent without deciding which is wanted (see that function).
// CALLED BY: FFT::outputBandwidth(), which the Info CHOP, the Info DAT and the popup read, and
//       test_rate_model() in tests/dsp_tests.cpp. Not used by bench/.
inline double throughput(const Parameters::Values& p, double cookDtMs)
{
	const int n_out = outputBinCountFrom(p);
	if (!(cookDtMs > 0.0)) return 0.0;
	return static_cast<double>(n_out) * 1000.0 / cookDtMs;
}
