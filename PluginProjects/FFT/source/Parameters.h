#ifndef PARAMETERS_H
#define PARAMETERS_H

/*
===========================================================================
                  PARAMETER MANAGEMENT SYSTEM
===========================================================================
Header File: Parameters.h

Architectural Design Pattern:
Based on Derivative's official C++ Operator Template architecture.
Encapsulates parameter names, display labels, menu string choices, and setup
logic into a dedicated module.
===========================================================================
*/

#include "CHOP_CPlusPlusBase.h"

namespace Parameters {
    // --- Page 1: Spectrum Parameters ---
    constexpr char EngineName[]       = "Engine";
    constexpr char EngineLabel[]      = "FFT Engine";

    constexpr char ScaleName[]        = "Scale";
    constexpr char ScaleLabel[]       = "Scale";

    constexpr char DisplaymaxName[]   = "Displaymax";
    constexpr char DisplaymaxLabel[]  = "Display Max Hz";

    constexpr char BinsName[]         = "Bins";
    constexpr char BinsLabel[]        = "Output Bins";

    constexpr char WarpName[]         = "Warp";
    constexpr char WarpLabel[]        = "Warp Blend";

    constexpr char LogfloorName[]     = "Logfloor";
    constexpr char LogfloorLabel[]    = "Log Floor Hz";

    constexpr char WinsamplesName[]   = "Winsamples";
    constexpr char WinsamplesLabel[]  = "Window Sampling";

    constexpr char PadName[]          = "Pad";
    constexpr char PadLabel[]         = "Zero-Pad Len";

    // --- Page 2: EQ Parameters ---
    constexpr char GaindbName[]       = "Gaindb";
    constexpr char GaindbLabel[]      = "High Boost dB";

    constexpr char CutoffhzName[]     = "Cutoffhz";
    constexpr char CutoffhzLabel[]    = "High Cutoff Hz";

    constexpr char LowgaindbName[]    = "Lowgaindb";
    constexpr char LowgaindbLabel[]   = "Low Boost dB";

    constexpr char LowcutoffhzName[]  = "Lowcutoffhz";
    constexpr char LowcutoffhzLabel[] = "Low Cutoff Hz";

    constexpr char QName[]            = "Q";
    constexpr char QLabel[]           = "EQ Q Factor";

    constexpr char AmountName[]       = "Amount";
    constexpr char AmountLabel[]      = "EQ Blend Amount";

    // --- Page 3: Window & Weighting Parameters ---
    constexpr char WindowName[]       = "Window";
    constexpr char WindowLabel[]      = "Window Type";

    constexpr char KaiserName[]       = "Kaiser";
    constexpr char KaiserLabel[]      = "Kaiser Beta";

    constexpr char WeightingName[]    = "Weighting";
    constexpr char WeightingLabel[]   = "Loudness Weighting";

    // --- Page 4: Loudness & Ballistics Parameters ---
    constexpr char LoudnessName[]     = "Loudness";
    constexpr char LoudnessLabel[]    = "Loudness Mode";

    constexpr char DbrangeName[]      = "Dbrange";
    constexpr char DbrangeLabel[]     = "dB Range Floor";

    constexpr char AttackName[]       = "Attack";
    constexpr char AttackLabel[]      = "Attack Speed";

    constexpr char ReleaseName[]      = "Release";
    constexpr char ReleaseLabel[]     = "Release Speed";

    constexpr char ResetName[]        = "Reset";
    constexpr char ResetLabel[]       = "Reset";

    // Registers custom parameters and UI controls with TouchDesigner's parameter manager
    void setup(TD::OP_ParameterManager* manager);
}

#endif // PARAMETERS_H
