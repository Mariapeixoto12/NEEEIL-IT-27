# Raspberry Pi Pico spectral-subtraction denoiser (CMSIS-DSP Q15)

This folder is the consolidated reference candidate for the RP2040 project. It
uses CMSIS-DSP for the FFT and fixed-point vector operations, and uses integer
arithmetic for the per-bin noise and gain calculations.

**Important scope note:** the firmware currently contains a synthetic DSP
benchmark and an FFT scaling diagnostic. It does not yet acquire microphone
audio or drive a speaker. Do not describe it as a complete live audio device.

## Why this is the reference candidate

- CMSIS-DSP `arm_cfft_q15()` performs the 512-point forward and inverse FFT.
- `arm_mult_q15()` performs analysis and synthesis window multiplication.
- `arm_cmplx_mult_real_q15()` applies the gain to each complex frequency bin.
- Power estimation, noise minima, gain calculation, and gain smoothing use
  integer/Q15 arithmetic inside the timed processing function.
- The inverse-transform scale is explicitly restored. CMSIS Q15 transforms
  use fixed scaling to prevent overflow; for a 512-point transform the
  forward/inverse round trip has a net factor of 1/512. The firmware restores
  that factor by multiplying inverse samples by 512 before synthesis
  windowing. A startup round-trip diagnostic checks that the restored RMS is
  within 20% of the input RMS before the benchmark is allowed to run.
- The benchmark separates average, best, worst and deadline misses.

This is a better baseline than the earlier CMSIS candidates because the
inverse scaling is explicit and checked at startup. The diagnostic is a scale
sanity check, not a complete numerical-accuracy certification.

## DSP configuration

| Setting | Value |
|---|---:|
| Sample rate | 16,000 Hz |
| Frame size | 512 samples (32 ms) |
| Hop size | 256 samples (16 ms) |
| Overlap | 50% |
| Window | Periodic square-root Hann, analysis and synthesis |
| FFT | CMSIS-DSP 512-point complex Q15 |
| Spectral subtraction factor | 4.0 |
| Minimum gain | 0.05 |
| Power smoothing | 0.60 old + 0.40 new |
| Frequency smoothing | 3-bin moving average, edge-clamped |
| Gain smoothing | 0.40 previous + 0.60 current |
| Noise estimate | Per-bin minima over 5 completed 16-frame blocks plus current block |
| Noise-history span | Up to about 95 frames, approximately 1.52 seconds |

The gain law is

`G[k] = max(1 - sqrt(4 * N[k] / P[k]), 0.05)`

where `P[k]` is smoothed observed spectral power and `N[k]` is the minimum
power estimate from the stored noise-history blocks. The same real-valued gain
is applied to positive-frequency bins and their conjugate mirrors, preserving
the Hermitian symmetry required for a real-valued inverse transform.

The minimum-history estimator is a simple, low-memory approximation. It can
mistake quiet speech for noise and is best evaluated with stationary and
changing noise, speech-active startup, and pauses. A clean initial noise-only
period is provided by the synthetic benchmark but cannot be assumed in a real
microphone application.

## Build dependencies

- Raspberry Pi Pico SDK (RP2040 target)
- `PICO_SDK_PATH` environment variable set to the SDK directory
- Arm GNU embedded toolchain supported by the Pico SDK
- CMake, Ninja (or Make), and Git
- Network access on the first configure/build, unless local source paths are
  provided

CMake fetches pinned versions:

- CMSIS-DSP `v1.18.0`
- CMSIS-Core from CMSIS_6 `v6.4.0`

The project defines `ARM_MATH_CM0PLUS`, selecting the Cortex-M0+ CMSIS-Core
headers and scalar CMSIS-DSP implementation. It does not enable M4/M7 DSP
instructions, Helium or Neon.

## Build for the original Raspberry Pi Pico (RP2040)

Run in this directory:

```sh
cmake -S . -B build -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build
```

If you use Make instead of Ninja, omit `-G Ninja`.

The firmware is:

`build/pico_spectral_subtraction_cmsis.uf2`

To flash it, hold BOOTSEL while connecting the Pico over USB, then copy the
UF2 file to the `RPI-RP2` drive. Open the USB serial/CDC port in a serial
monitor (115200 baud is a suitable monitor setting). Press RESET if the board
starts before the serial monitor is open.

To use local checkouts instead of downloading dependencies:

```sh
cmake -S . -B build -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release -G Ninja \
  -DCMSIS_DSP_PATH=/path/to/CMSIS-DSP \
  -DCMSIS_CORE_PATH=/path/to/CMSIS_6/CMSIS/Core
cmake --build build
```

## Run in Wokwi

The included `wokwi.toml` points to the same firmware output path. Open this
folder in the Wokwi VS Code extension, build it, and start the simulator.
The synthetic benchmark is useful for regression comparisons, but Wokwi timing
must not be treated as a substitute for timing on the physical Pico.

## What the firmware tests

1. It initializes CMSIS-DSP's 512-point Q15 CFFT.
2. It performs a forward/inverse transform on a deterministic two-tone signal.
3. It restores the Q15 transform scale and compares input and reconstructed
   RMS. If the restored RMS is outside 80–120% of the input RMS, it stops
   instead of producing a misleading timing result.
4. It processes 188 hops: approximately 0.992 s of synthetic noise-only audio,
   followed by approximately 2.008 s of a 440 Hz tone mixed with deterministic
   broadband noise.
5. It times only `denoiser_process_hop()` using `time_us_32()`. Signal
   generation, diagnostic RMS calculations and serial printing are excluded.
6. It reports system clock, round-trip RMS diagnostic, per-hop timing, average,
   best, worst, and hops above the 16,000 us budget.

At 16 kHz, a 256-sample hop represents 16 ms. The deadline is therefore
16,000 us. A timing pass means the worst observed hop is less than or equal to
that deadline during this 188-hop synthetic run. It does not yet prove
continuous live-audio stability or account for ADC/DMA/PWM/I2S overhead.

## Audio quality validation still required

The current synthetic signal is useful for exercising the code path and
checking timing, but it cannot establish noise-reduction quality. For a
credible DSP evaluation:

1. Use clean speech and several separate noise recordings (fan/ventilation,
   broadband hiss, crowd/cafeteria, intermittent impacts).
2. Create identical noisy mixtures for both the previous and current
   implementations.
3. Compare output SNR against the clean speech reference where available.
4. Listen for speech distortion, musical noise, pumping and transient leakage.
5. Test with speech present from startup, rather than always giving the noise
   estimator a quiet calibration interval.
6. Only then integrate the microphone ADC/audio codec and DAC/PWM/I2S output.
7. Re-run the timing benchmark on the physical Pico with the final I/O path
   active and retain a margin below the 16 ms hop deadline.

Do not interpret RMS reduction on its own as an improvement: reducing speech
and noise together can also lower RMS.

## Source references

- [CMSIS-DSP Q15 complex FFT documentation](https://arm-software.github.io/CMSIS-DSP/main/group__ComplexFFTQ15.html)
- [CMSIS-DSP Q15 complex-by-real multiplication](https://arm-software.github.io/CMSIS-DSP/main/group__CmplxByRealMult.html)
- [CMSIS-DSP upstream repository and build guidance](https://github.com/ARM-software/CMSIS-DSP)
- [RP2040 technical specifications](https://www.raspberrypi.com/products/rp2040/specifications/)

CMSIS-DSP is distributed under the Apache License 2.0. See the upstream
repository for license text and notices.
