@echo off
rem Installs the FFT Custom CHOP from this folder (the extracted TD_Custom_FFT-Windows.zip) into
rem Documents\Derivative\Plugins\FFT. Double-click it, or run it from a command prompt with options:
rem   Install.cmd -OneMKL        also install the Intel oneMKL backend (166 MB download from nuget.org)
rem   Install.cmd -Uninstall     remove the plugin and the examples
rem   Install.cmd -?             every option (it is install.ps1's help)
rem ExecutionPolicy Bypass applies to this one PowerShell process only; nothing is changed system-wide.
if "%~1"=="-?" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-Help '%~dp0install.ps1' -Detailed"
  pause
  exit /b 0
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set "rc=%ERRORLEVEL%"
echo.
pause
exit /b %rc%
