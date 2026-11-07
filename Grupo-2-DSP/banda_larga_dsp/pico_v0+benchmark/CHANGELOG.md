# Changes in the consolidated reference candidate

- Consolidated on CMSIS-DSP Q15 CFFT and fixed-point spectral gain processing.
- Explicitly compensates the 512-point Q15 forward/inverse transform scaling
  by restoring nine bits after inverse FFT.
- Added a startup FFT round-trip RMS scale check before the timing benchmark.
- Corrected the minimum-gain threshold to use a ceiling at the clamp boundary.
- Reports the actual system clock and average, best, worst and over-budget hop
  counts separately.
- Kept the original frame, hop, spectral-subtraction and smoothing parameters
  to retain comparability with the prior prototype.
