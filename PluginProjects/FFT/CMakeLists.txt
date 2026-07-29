# ===========================================================================
#                    CMake Build Script for TouchDesigner FFT CHOP
# ===========================================================================
# Targeted Output: Shared Dynamic Link Library (FFT.dll)
# Dependencies: TouchDesigner C++ SDK, FFTW3 (64-bit Windows single-precision libfftw3f-3)
# ===========================================================================

cmake_minimum_required (VERSION 3.21)

# Enable Hot Reload for MSVC compilers if supported by CMake policy CMP0141
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# Default to Release build configuration if unassigned
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo")
endif()

# Require Modern C++17 Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS ON)

# Configure output directories for binary artifacts
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/Debug)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/Debug)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/Debug)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/Release)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/Release)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/Release)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/RelWithDebInfo)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/RelWithDebInfo)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/RelWithDebInfo)
endif()

project (FFT LANGUAGES CXX CUDA)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_ARCHITECTURES 89)

find_package(CUDAToolkit REQUIRED)

# PluginBuilder SDK paths
set(PLUGIN_BUILDER_DIR "C:\\Users\\Z\\Downloads\\PROJECTS\\TD_PROJECTS\\PluginBuilder\\PluginBuilder_V2" CACHE PATH "Path to PluginBuilder directory" FORCE)
set(PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../__Plugins__/FFT")
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/source)

set(INCLUDE_DIR "${PLUGIN_BUILDER_DIR}/include")
message(STATUS "INCLUDE_DIR: ${INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# FFTW3 (Single-Precision libfftw3f-3) Discovery & Import Library Setup
# ---------------------------------------------------------------------------
set(FFTW3_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/fftw3")
set(FFTW3_GLOBAL_DIR "${PLUGIN_BUILDER_DIR}/3rdParty/fftw3")

# Locate fftw3.h header file
find_path(FFTW3_INCLUDE_DIR NAMES fftw3.h
    PATHS
        "${FFTW3_LOCAL_DIR}/include"
        "${FFTW3_LOCAL_DIR}"
        "${FFTW3_GLOBAL_DIR}/include"
        "${FFTW3_GLOBAL_DIR}"
)

# Auto-generate MSVC import library (libfftw3f-3.lib) from definition file (libfftw3f-3.def) if missing
foreach(dir IN ITEMS "${FFTW3_LOCAL_DIR}/lib" "${FFTW3_LOCAL_DIR}" "${FFTW3_GLOBAL_DIR}/lib" "${FFTW3_GLOBAL_DIR}")
    if(EXISTS "${dir}/libfftw3f-3.def" AND NOT EXISTS "${dir}/libfftw3f-3.lib" AND MSVC)
        find_program(MSVC_LIB_EXE lib.exe)
        if(MSVC_LIB_EXE)
            message(STATUS "Generating libfftw3f-3.lib from libfftw3f-3.def in ${dir}")
            execute_process(
                COMMAND "${MSVC_LIB_EXE}" /machine:x64 "/def:${dir}/libfftw3f-3.def" "/out:${dir}/libfftw3f-3.lib"
                WORKING_DIRECTORY "${dir}"
            )
        endif()
    endif()
endforeach()

# Locate single-precision FFTW3 import library (libfftw3f-3.lib)
find_library(FFTW3F_LIBRARY
    NAMES libfftw3f-3 fftw3f libfftw3f
    PATHS
        "${FFTW3_LOCAL_DIR}/lib"
        "${FFTW3_LOCAL_DIR}"
        "${FFTW3_GLOBAL_DIR}/lib"
        "${FFTW3_GLOBAL_DIR}"
)

# Locate single-precision FFTW3 runtime DLL (libfftw3f-3.dll)
find_file(FFTW3F_DLL
    NAMES libfftw3f-3.dll
    PATHS
        "${FFTW3_LOCAL_DIR}/bin"
        "${FFTW3_LOCAL_DIR}/lib"
        "${FFTW3_LOCAL_DIR}"
        "${FFTW3_GLOBAL_DIR}/bin"
        "${FFTW3_GLOBAL_DIR}/lib"
        "${FFTW3_GLOBAL_DIR}"
)

if(FFTW3_INCLUDE_DIR)
    message(STATUS "Found FFTW3 Header: ${FFTW3_INCLUDE_DIR}")
else()
    message(WARNING "FFTW3 header (fftw3.h) not found in 3rdParty/fftw3.")
endif()

if(FFTW3F_LIBRARY)
    message(STATUS "Found FFTW3 Library: ${FFTW3F_LIBRARY}")
else()
    message(WARNING "FFTW3 single-precision library (libfftw3f-3.lib) not found.")
endif()

# Collect all C++ source and header files
file(GLOB_RECURSE PROJ_SOURCE_FILES 
"${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.c" "${SOURCE_DIR}/*.cu" "${SOURCE_DIR}/*.h" "${SOURCE_DIR}/*.cuh")

# Declare shared library target for TouchDesigner
add_library(FFT SHARED ${PROJ_SOURCE_FILES})
target_include_directories(FFT PRIVATE ${SOURCE_DIR} ${INCLUDE_DIR})

if(FFTW3_INCLUDE_DIR)
    target_include_directories(FFT PRIVATE ${FFTW3_INCLUDE_DIR})
endif()

if(FFTW3F_LIBRARY)
    target_link_libraries(FFT PRIVATE ${FFTW3F_LIBRARY})
endif()

target_link_libraries(FFT PRIVATE CUDA::cufft CUDA::cudart)

# MSVC Compiler & Linker Optimization Flags for Ultra Real-Time Performance
if(DEFINED ENV{PLUGINBUILDER_BUILD})
  message(STATUS "PluginBuilder is building FFT")
else()  
  if(MSVC)
    # /O2: Maximize speed optimization
    # /Oi: Enable intrinsic functions
    # /Ot: Favor fast code over small code
    # /fp:fast: Enable fast floating-point math optimizations
    # /arch:AVX2: Generate 256-bit AVX2 & FMA vector instructions
    target_compile_options(FFT PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/O2> $<$<COMPILE_LANGUAGE:CXX>:/Oi> $<$<COMPILE_LANGUAGE:CXX>:/Ot> $<$<COMPILE_LANGUAGE:CXX>:/fp:fast> $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX2>)
    target_compile_options(FFT PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/Zi>)
    target_link_options(FFT PUBLIC "/INCREMENTAL")
  endif()

  # Post-build command: Automatically deploy FFT.dll and libfftw3f-3.dll to TouchDesigner __Plugins__/FFT/ output folder
  add_custom_command(TARGET FFT POST_BUILD
    COMMAND powershell -Command "if (Test-Path '${PLUGIN_DIR}/FFT.dll.old') { Remove-Item '${PLUGIN_DIR}/FFT.dll.old' -Force -ErrorAction SilentlyContinue }; if (Test-Path '${PLUGIN_DIR}/FFT.dll') { Move-Item '${PLUGIN_DIR}/FFT.dll' '${PLUGIN_DIR}/FFT.dll.old' -Force -ErrorAction SilentlyContinue }; Copy-Item '$<TARGET_FILE:FFT>' '${PLUGIN_DIR}/FFT.dll' -Force; if ('${FFTW3F_DLL}' -ne '' -and (Test-Path '${FFTW3F_DLL}')) { Copy-Item '${FFTW3F_DLL}' '${PLUGIN_DIR}/libfftw3f-3.dll' -Force }"
    VERBATIM)
endif()
