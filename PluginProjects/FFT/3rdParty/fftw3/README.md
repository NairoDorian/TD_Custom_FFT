# FFTW3 for Windows 64-bit

This directory stores the FFTW3 (single-precision `libfftw3f-3`) 64-bit Windows pre-compiled binaries.

## Download & Setup Instructions

1. Download the 64-bit Windows binary zip from official website:
   https://www.fftw.org/install/windows.html (e.g., `fftw-3.3.5-dll64.zip`)

2. Extract files into the following layout:
   - `3rdParty/fftw3/include/fftw3.h`
   - `3rdParty/fftw3/lib/libfftw3f-3.def`
   - `3rdParty/fftw3/lib/libfftw3f-3.lib` (Generated using `lib /machine:x64 /def:libfftw3f-3.def`)
   - `3rdParty/fftw3/bin/libfftw3f-3.dll`

3. Import Library Generation (if `.lib` is not yet generated):
   Open Visual Studio Developer Command Prompt, navigate to `3rdParty/fftw3/lib` and run:
   ```cmd
   lib /machine:x64 /def:libfftw3f-3.def /out:libfftw3f-3.lib
   ```
   *Note: If building with MSVC via CMake, CMake will attempt to automatically run `lib.exe` to generate `libfftw3f-3.lib` if `libfftw3f-3.def` is present.*
