# Examples

Projects that use the **FFT Custom** CHOP. They need the plugin installed (see [Install](../README.md#install));
the installers also copy them into `Documents\TD_Custom_FFT\Examples\`.

## Circular_Audio_Spectrum.toe

A live audio spectrum drawn as a ring, built with POPs: the audio input goes through FFT Custom, the spectrum is
shaped with Math/Trail CHOPs, turned into points around a circle (`POP chopto` → copies → transforms) and viewed
through an OP Viewer. `/project1` shows the result; the network is in `/project1/base1`.

What it uses from the operator:

- **Multi-FFT** (`Fftcustom7`, the node the visual reads): layers from 1024 to 32768 points averaged on one output
  grid, so bass keeps its resolution and transients stay sharp.
- **Fixed output bins** with a **log-warped** axis (Warp Blend 0.93, cubic interpolation), so the ring spends its
  points evenly across the octaves instead of on the top of the spectrum.
- **C-weighting**, a Hamming window, the input EQ (+24 dB), and a 110 ms release (Ballistics in milliseconds).
- `Fftcustom1`…`Fftcustom6` are bypassed experiments with single-FFT settings at growing window lengths.
  `switch3` picks the spectrum: input 0 (default) is the Multi-FFT node; input 1 merges TouchDesigner's built-in
  **Audio Spectrum** CHOP with `Fftcustom1`, for a side-by-side comparison.

### Before you press play

1. **Pick your audio input.** `/project1/base1/audiodevin1` (Audio Device In) was saved listening to a virtual
   cable on the author's machine (*VB-Audio Hi-Fi Cable*). On your machine choose your own device on its
   **Device** parameter: a microphone or line input, or a loopback/virtual cable to visualise what the computer
   plays. To try it without any device, wire an **Audio File In** CHOP into `in1` instead
   (TouchDesigner's samples folder has music in `Samples/Audio/`).
2. **FFT Backend is on** (Intel oneMKL) on every FFT Custom node. Without oneMKL installed the nodes run on
   FFTW3, produce the same spectrum, and show a warning saying so. Either install the optional oneMKL component
   (Setup.exe, or `install.ps1 -OneMKL`) or turn **FFT Backend** off on the Performance page.

The file is saved by TouchDesigner 2025.33230. Opening it in an older TouchDesigner may drop newer operators
(the POP family needs 2025.30000 or later).
