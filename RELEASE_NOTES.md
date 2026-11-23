**TD Custom FFT v2.14.0: the first public release of the FFT Custom CHOP for TouchDesigner.**

A real-time audio spectrum operator: audio in, a psychoacoustically scaled magnitude spectrum out (Log / Mel / ERB /
Bark / Chroma axes, Multi-FFT, equal-loudness weighting, dB, ballistics), with the FFT on its own thread and nothing
that allocates, locks or blocks on TouchDesigner's cook thread.

**Install**

- **`TD_Custom_FFT-Setup.exe`** (recommended). *Standard* installs the operator and the example project; *Full* also
  downloads the optional Intel oneMKL backend from nuget.org (166 MB, checked against a pinned SHA-256). Per user, no
  administrator rights. Close TouchDesigner first. The installer is unsigned: if SmartScreen warns, click
  *More info*, then *Run anyway*.
- **PowerShell:** `irm https://raw.githubusercontent.com/NairoDorian/TD_Custom_FFT/main/installer/install.ps1 | iex`
- **`TD_Custom_FFT-Windows.zip`** for a manual or offline install: run `Install.cmd`, or copy the `FFT` folder into
  `Documents\Derivative\Plugins\`.

Then press Tab in TouchDesigner and create **CHOP > FFT Custom**.

**Requirements:** Windows 10/11 64-bit, a CPU with AVX2 + FMA, TouchDesigner 2025.30000 or later, the current
Microsoft Visual C++ Redistributable (x64); the installers check each of these.

**New in this version**

- The installers above, and an example project: `Circular_Audio_Spectrum.toe`, a live spectrum ring built with POPs.
  Pick your own audio input in it first (see `examples/README.md` in the repository).
- A node with **FFT Backend** on but no oneMKL installed now shows a warning saying it runs on FFTW3, instead of
  silently running slower.

**Also attached:** `install.ps1`, `fftw-3.3.11.tar.gz` and its one-line patch (the corresponding source of the bundled
FFTW DLL, GPL), and `SHA256SUMS.txt`.

License: GPL-3.0-or-later with a permission to combine the plugin with the TouchDesigner SDK (`LICENSE`, `NOTICE.md`).
Full history: `CHANGELOG.md`.
