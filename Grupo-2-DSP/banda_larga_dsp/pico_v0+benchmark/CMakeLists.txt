cmake_minimum_required(VERSION 3.20)

if (NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Install the Pico SDK and set this environment variable.")
endif()
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake) # import the sdk

project(pico_spectral_subtraction_cmsis C CXX ASM)  # create the cmake project
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init() # initialize the sdk
include(FetchContent)

# CMSIS-DSP needs the matching CMSIS-Core include tree. Versions are pinned.
set(CMSIS_CORE_PATH "" CACHE PATH "Optional path to CMSIS_6/CMSIS/Core")
set(CMSIS_DSP_PATH "" CACHE PATH "Optional path to a CMSIS-DSP checkout")

if (CMSIS_CORE_PATH STREQUAL "")
    FetchContent_Declare(
        cmsis_core
        GIT_REPOSITORY https://github.com/ARM-software/CMSIS_6.git
        GIT_TAG        v6.4.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(cmsis_core)
    if (NOT cmsis_core_POPULATED)
        FetchContent_Populate(cmsis_core)
    endif()
    set(CMSIS_CORE_PATH "${cmsis_core_SOURCE_DIR}/CMSIS/Core")
endif()

if (CMSIS_DSP_PATH STREQUAL "")
    FetchContent_Declare(
        cmsis_dsp
        GIT_REPOSITORY https://github.com/ARM-software/CMSIS-DSP.git
        GIT_TAG        v1.18.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_GetProperties(cmsis_dsp)
    if (NOT cmsis_dsp_POPULATED)
        FetchContent_Populate(cmsis_dsp)
    endif()
    set(CMSIS_DSP_PATH "${cmsis_dsp_SOURCE_DIR}")
endif()

if (NOT EXISTS "${CMSIS_CORE_PATH}/Include/core_cm0plus.h")
    message(FATAL_ERROR "CMSIS_CORE_PATH must point to CMSIS_6/CMSIS/Core")
endif()
if (NOT EXISTS "${CMSIS_DSP_PATH}/Source/CMakeLists.txt")
    message(FATAL_ERROR "CMSIS_DSP_PATH must point to the CMSIS-DSP repository root")
endif()

# RP2040 is an Arm Cortex-M0+. This selects CMSIS-Core's M0+ headers and the
# scalar CMSIS-DSP implementation (no M4/M7 DSP or Helium intrinsics).
add_compile_definitions(ARM_MATH_CM0PLUS)
set(CMSISCORE "${CMSIS_CORE_PATH}" CACHE PATH "CMSIS-Core parent directory" FORCE)
set(DISABLEFLOAT16 ON CACHE BOOL "Disable unused float16 kernels" FORCE)
set(LOOPUNROLL ON CACHE BOOL "Enable CMSIS-DSP loop unrolling" FORCE)
set(CMSISDSP_INSTALL OFF CACHE BOOL "Do not install CMSIS-DSP from this project" FORCE)

add_subdirectory("${CMSIS_DSP_PATH}/Source" "${CMAKE_CURRENT_BINARY_DIR}/cmsis_dsp")

add_executable(pico_spectral_subtraction_cmsis
    main.c
    denoiser.c
)

target_include_directories(pico_spectral_subtraction_cmsis PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
)

target_compile_options(pico_spectral_subtraction_cmsis PRIVATE
    -O3
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
)
target_compile_options(CMSISDSP PRIVATE -O3 -ffunction-sections -fdata-sections)

target_link_libraries(pico_spectral_subtraction_cmsis PRIVATE
    pico_stdlib
    hardware_clocks
    CMSISDSP
    m
)

pico_enable_stdio_usb(pico_spectral_subtraction_cmsis 1)
pico_enable_stdio_uart(pico_spectral_subtraction_cmsis 0)
pico_add_extra_outputs(pico_spectral_subtraction_cmsis)
