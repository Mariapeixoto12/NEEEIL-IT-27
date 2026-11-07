cmake_minimum_required(VERSION 3.20)

if (NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Install the Raspberry Pi Pico SDK first.")
endif()
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_usb_denoiser C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()
include(FetchContent)

# Select the CMSIS core definition that matches the board selected in CMake.
if (PICO_PLATFORM STREQUAL "rp2040")
    add_compile_definitions(ARM_MATH_CM0PLUS)
elseif (PICO_PLATFORM MATCHES "^rp2350-arm")
    # Cortex-M33 uses the Armv8-M Mainline CMSIS core definition.
    add_compile_definitions(ARM_MATH_ARMV8MML)
else()
    message(FATAL_ERROR "This project currently supports RP2040 and RP2350 Arm builds, not ${PICO_PLATFORM}.")
endif()

set(CMSIS_CORE_PATH "" CACHE PATH "Optional path to CMSIS_6/CMSIS/Core")
set(CMSIS_DSP_PATH "" CACHE PATH "Optional path to a CMSIS-DSP checkout")

if (CMSIS_CORE_PATH STREQUAL "")
    FetchContent_Declare(
        cmsis_core
        GIT_REPOSITORY https://github.com/ARM-software/CMSIS_6.git
        GIT_TAG v6.4.0
        GIT_SHALLOW TRUE
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
        GIT_TAG v1.18.0
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(cmsis_dsp)
    if (NOT cmsis_dsp_POPULATED)
        FetchContent_Populate(cmsis_dsp)
    endif()
    set(CMSIS_DSP_PATH "${cmsis_dsp_SOURCE_DIR}")
endif()

if (PICO_PLATFORM STREQUAL "rp2040")
    set(CMSIS_CORE_HEADER "${CMSIS_CORE_PATH}/Include/core_cm0plus.h")
else()
    set(CMSIS_CORE_HEADER "${CMSIS_CORE_PATH}/Include/core_cm33.h")
endif()

if (NOT EXISTS "${CMSIS_CORE_HEADER}")
    message(FATAL_ERROR "CMSIS Core header not found: ${CMSIS_CORE_HEADER}")
endif()
if (NOT EXISTS "${CMSIS_DSP_PATH}/Source/CMakeLists.txt")
    message(FATAL_ERROR "CMSIS_DSP_PATH must point to the root of CMSIS-DSP")
endif()

set(CMSISCORE "${CMSIS_CORE_PATH}" CACHE PATH "CMSIS-Core path" FORCE)
set(DISABLEFLOAT16 ON CACHE BOOL "Disable unused float16 kernels" FORCE)
set(LOOPUNROLL ON CACHE BOOL "Enable CMSIS-DSP loop unrolling" FORCE)
set(CMSISDSP_INSTALL OFF CACHE BOOL "Do not install CMSIS-DSP" FORCE)
add_subdirectory("${CMSIS_DSP_PATH}/Source" "${CMAKE_CURRENT_BINARY_DIR}/cmsis_dsp")

add_executable(pico_usb_denoiser
    main.c
    denoiser.c
)

target_include_directories(pico_usb_denoiser PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_options(pico_usb_denoiser PRIVATE
    -O3
    -Wall
    -Wextra
    -ffunction-sections
    -fdata-sections
)
target_compile_options(CMSISDSP PRIVATE -O3 -ffunction-sections -fdata-sections)

target_link_libraries(pico_usb_denoiser PRIVATE
    pico_stdlib
    hardware_clocks
    CMSISDSP
    m
)

# One USB CDC port carries both the text commands and the raw audio bytes.
pico_enable_stdio_usb(pico_usb_denoiser 1)
pico_enable_stdio_uart(pico_usb_denoiser 0)
pico_add_extra_outputs(pico_usb_denoiser)
