@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
REM Run from a folder that contains the extracted fftw-3.3.11 tree with fftw-3.3.11-cmake-version.patch applied.
cd /d "%~dp0"
cmake -S fftw-3.3.11 -B fftw-build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON ^
  -DBUILD_TESTS=OFF ^
  -DENABLE_FLOAT=ON ^
  -DENABLE_LONG_DOUBLE=OFF ^
  -DENABLE_SSE2=ON ^
  -DENABLE_AVX=ON ^
  -DENABLE_AVX2=ON ^
  -DENABLE_THREADS=OFF ^
  -DENABLE_OPENMP=OFF ^
  -DDISABLE_FORTRAN=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1
cmake --build fftw-build
