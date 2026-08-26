# FFTW3 for Windows 64-bit (single precision)

Only the single-precision runtime is vendored:

```
3rdParty/fftw3/
├── include/fftw3.h
├── lib/libfftw3f-3.def      <- from fftw-3.3.5-dll64.zip
├── lib/libfftw3f-3.lib      <- MSVC import library (generated from the .def by td_plugin_use_fftw3 / lib.exe)
└── bin/libfftw3f-3.dll      <- runtime, copied next to FFT.dll by the build
```

`td_plugin_use_fftw3(FFT)` in `CMakeLists.txt` (from `PluginBuilder_V2/cmake/TDPlugin.cmake`) locates these
files, generates the `.lib` from the `.def` when it is missing, links the plugin and stages/deploys the DLL.

Source: https://www.fftw.org/install/windows.html (`fftw-3.3.5-dll64.zip`, MinGW build with SSE2/AVX codelets).
For an AVX2/FMA-enabled FFTW (~10–25 % faster transforms) build 3.3.10 with `-DENABLE_AVX2=ON`
(e.g. `vcpkg install fftw3[avx2]:x64-windows`) and drop the resulting `libfftw3f-3.*` files here.

License: FFTW is GPL. See §"License / third party" in the project README.
