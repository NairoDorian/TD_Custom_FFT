/* Shared Use License: This file is owned by Derivative Inc. (Derivative)
* and can only be used, and/or modified for use, in conjunction with
* Derivative's TouchDesigner software, and only if you are a licensee who has
* accepted Derivative's TouchDesigner license or assignment agreement
* (which also govern the use of this file). You may share or redistribute
* a modified version of this file provided the following conditions are met:
*
* 1. The shared file or redistribution must retain the information set out
* above and this list of conditions.
* 2. Derivative's name (Derivative Inc.) or its trademarks may not be used
* to endorse or promote products derived from this file without specific
* prior written permission from Derivative.
*/

/*
===========================================================================
                  PARAMETER SETUP IMPLEMENTATION
===========================================================================
Source File: Parameters.cpp

Functional Overview:
Registers 21 custom parameters organized into 4 distinct TouchDesigner UI pages:
1. Spectrum: Frequency scale (Log, Mel, ERB, Bark, Chroma, Linear, Melog), output bin resolution, log floor, windowing samples, zero-padding size.
2. EQ: High shelf & Low shelf parametric boost/cut, cutoff frequencies, Q factor, wet/dry blend.
3. Window & Weighting: Window types (Kaiser, Hann, Hamming, Blackman, Blackman-Harris, Rectangular), Kaiser beta, ISO 226 loudness weighting curves.
4. Loudness & Ballistics: Magnitude/dB/dB-normalized modes, dB dynamic range floor, asymmetric attack & release dynamic smoothing, pulse reset.
===========================================================================
*/

#include "Parameters.h"

namespace Parameters {

void setup(TD::OP_ParameterManager* manager)
{
	// --- Page 1: Spectrum ---
	{
		TD::OP_StringParameter sp;
		sp.page = "Spectrum";
		sp.name = EngineName;
		sp.label = EngineLabel;
		sp.defaultValue = "Fftw3";

		const char *names[]  = { "Fftw3", "Mkl", "Cufft", "Vkfft", "Cufftdx" };
		const char *labels[] = { "CPU (FFTW3 AVX2)", "CPU (Intel oneMKL / IPP)", "GPU (NVIDIA cuFFT)", "GPU (VkFFT CUDA/Vulkan)", "GPU Fused (cuFFTDx)" };
		manager->appendMenu(sp, 5, names, labels);
	}
	{
		TD::OP_StringParameter sp;
		sp.page = "Spectrum";
		sp.name = ScaleName;
		sp.label = ScaleLabel;
		sp.defaultValue = "Log";

		const char *names[]  = { "Log", "Mel", "ERB", "Bark", "Chroma", "Linear", "Melog" };
		const char *labels[] = { "Logarithmic", "Mel Scale", "ERB Scale", "Bark Scale", "Chroma / Pitch", "Linear Scale", "Mel + Log Blend" };
		manager->appendMenu(sp, 7, names, labels);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Spectrum";
		np.name = DisplaymaxName;
		np.label = DisplaymaxLabel;
		np.defaultValues[0] = 24000.0;
		np.minSliders[0] = 100.0;
		np.maxSliders[0] = 48000.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Spectrum";
		np.name = BinsName;
		np.label = BinsLabel;
		np.defaultValues[0] = 16384;
		np.minSliders[0] = 256;
		np.maxSliders[0] = 32768;
		manager->appendInt(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Spectrum";
		np.name = WarpName;
		np.label = WarpLabel;
		np.defaultValues[0] = 0.963;
		np.minSliders[0] = 0.0;
		np.maxSliders[0] = 1.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Spectrum";
		np.name = LogfloorName;
		np.label = LogfloorLabel;
		np.defaultValues[0] = 20.0;
		np.minSliders[0] = 1.0;
		np.maxSliders[0] = 500.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Spectrum";
		np.name = WinsamplesName;
		np.label = WinsamplesLabel;
		np.defaultValues[0] = 3175;
		np.minSliders[0] = 1;
		np.maxSliders[0] = 32768;
		manager->appendInt(np);
	}
	{
		TD::OP_StringParameter sp;
		sp.page = "Spectrum";
		sp.name = PadName;
		sp.label = PadLabel;
		sp.defaultValue = "32768";

		const char *names[]  = { "1024", "2048", "4096", "8192", "16384", "32768", "65536" };
		const char *labels[] = { "1K Bins", "2K Bins", "4K Bins", "8K Bins", "16K Bins", "32K Bins", "64K Bins" };
		manager->appendMenu(sp, 7, names, labels);
	}

	// --- Page 2: EQ ---
	{
		TD::OP_NumericParameter np;
		np.page = "EQ";
		np.name = GaindbName;
		np.label = GaindbLabel;
		np.defaultValues[0] = 6.0;
		np.minSliders[0] = -24.0;
		np.maxSliders[0] = 24.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "EQ";
		np.name = CutoffhzName;
		np.label = CutoffhzLabel;
		np.defaultValues[0] = 1000.0;
		np.minSliders[0] = 20.0;
		np.maxSliders[0] = 20000.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "EQ";
		np.name = LowgaindbName;
		np.label = LowgaindbLabel;
		np.defaultValues[0] = 0.0;
		np.minSliders[0] = -24.0;
		np.maxSliders[0] = 24.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "EQ";
		np.name = LowcutoffhzName;
		np.label = LowcutoffhzLabel;
		np.defaultValues[0] = 200.0;
		np.minSliders[0] = 20.0;
		np.maxSliders[0] = 5000.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "EQ";
		np.name = QName;
		np.label = QLabel;
		np.defaultValues[0] = 0.707;
		np.minSliders[0] = 0.1;
		np.maxSliders[0] = 4.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "EQ";
		np.name = AmountName;
		np.label = AmountLabel;
		np.defaultValues[0] = 1.0;
		np.minSliders[0] = 0.0;
		np.maxSliders[0] = 5.0;
		manager->appendFloat(np);
	}

	// --- Page 3: Window & Weighting ---
	{
		TD::OP_StringParameter sp;
		sp.page = "Window & Weighting";
		sp.name = WindowName;
		sp.label = WindowLabel;
		sp.defaultValue = "Kaiser";

		const char *names[]  = { "Kaiser", "Hann", "Hamming", "Blackman", "Blackmanharris", "Rectangular" };
		const char *labels[] = { "Kaiser (Beta control)", "Hann Window", "Hamming Window", "Blackman Window", "Blackman-Harris (92dB)", "Rectangular (Flat)" };
		manager->appendMenu(sp, 6, names, labels);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Window & Weighting";
		np.name = KaiserName;
		np.label = KaiserLabel;
		np.defaultValues[0] = 15;
		np.minSliders[0] = 1;
		np.maxSliders[0] = 55;
		manager->appendInt(np);
	}
	{
		TD::OP_StringParameter sp;
		sp.page = "Window & Weighting";
		sp.name = WeightingName;
		sp.label = WeightingLabel;
		sp.defaultValue = "Off";

		const char *names[]  = { "Off", "Aweighting", "Cweighting", "Itu468" };
		const char *labels[] = { "Off (Flat)", "A-Weighting (IEC 61672)", "C-Weighting (High SPL)", "ITU-R 468 (Noise standard)" };
		manager->appendMenu(sp, 4, names, labels);
	}

	// --- Page 4: Loudness & Ballistics ---
	{
		TD::OP_StringParameter sp;
		sp.page = "Loudness & Ballistics";
		sp.name = LoudnessName;
		sp.label = LoudnessLabel;
		sp.defaultValue = "Off";

		const char *names[]  = { "Off", "Db", "Dbnorm" };
		const char *labels[] = { "Off (Linear Magnitude)", "dB (Decibels)", "dB Normalized (0.0 to 1.0)" };
		manager->appendMenu(sp, 3, names, labels);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Loudness & Ballistics";
		np.name = DbrangeName;
		np.label = DbrangeLabel;
		np.defaultValues[0] = 80.0;
		np.minSliders[0] = 10.0;
		np.maxSliders[0] = 160.0;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Loudness & Ballistics";
		np.name = AttackName;
		np.label = AttackLabel;
		np.defaultValues[0] = 0.0;
		np.minSliders[0] = 0.0;
		np.maxSliders[0] = 0.99;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Loudness & Ballistics";
		np.name = ReleaseName;
		np.label = ReleaseLabel;
		np.defaultValues[0] = 0.0;
		np.minSliders[0] = 0.0;
		np.maxSliders[0] = 0.99;
		manager->appendFloat(np);
	}
	{
		TD::OP_NumericParameter np;
		np.page = "Loudness & Ballistics";
		np.name = ResetName;
		np.label = ResetLabel;
		manager->appendPulse(np);
	}
}

} // namespace Parameters
