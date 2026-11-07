#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "denoiser.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define TEST_HOPS       188u
#define NOISE_ONLY_HOPS 62u
#define HOP_BUDGET_US   16000u
#define PI_F            3.14159265358979323846f

static Denoiser denoiser;
static int16_t input_hop[DENOISER_HOP_SIZE];
static int16_t output_hop[DENOISER_HOP_SIZE];
static q15_t fft_test_buffer[2u * DENOISER_FRAME_SIZE];
static int16_t fft_test_input[DENOISER_FRAME_SIZE];
static uint32_t rng_state = 0x12345678u;

static uint32_t random_u32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int16_t make_test_sample(uint32_t sample_index, bool include_tone)
{
    const int32_t random_signed = (int32_t)(random_u32() & 0xFFFFu) - 32768;
    const int32_t noise = (random_signed * 1200) / 32768;

    int32_t tone = 0;
    if (include_tone) {
        const float phase = 2.0f * PI_F * 440.0f *
                            (float)sample_index /
                            (float)DENOISER_SAMPLE_RATE_HZ;
        tone = (int32_t)(7000.0f * sinf(phase));
    }

    int32_t mixed = tone + noise;
    if (mixed > 32767) mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    return (int16_t)mixed;
}

static uint32_t rms_q15(const int16_t *samples, uint32_t count)
{
    uint64_t sum_squares = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t sample = samples[i];
        sum_squares += (uint64_t)(sample * sample);
    }
    return (uint32_t)(sqrtf((float)sum_squares / (float)count) + 0.5f);
}

/*
 * Diagnostic for CMSIS Q15 fixed scaling. It checks a forward/inverse CFFT
 * round trip before the denoiser benchmark. The expected net correction for
 * N=512 is a factor of 512 (9 bits).
 */
static bool fft_roundtrip_diagnostic(uint32_t *input_rms_out,
                                     uint32_t *output_rms_out,
                                     uint32_t *error_rms_out,
                                     uint32_t *max_error_out)
{
    for (uint32_t i = 0; i < DENOISER_FRAME_SIZE; ++i) {
        const float phase_a = 2.0f * PI_F * 14.0f * (float)i /
                              (float)DENOISER_FRAME_SIZE;
        const float phase_b = 2.0f * PI_F * 32.0f * (float)i /
                              (float)DENOISER_FRAME_SIZE;
        int32_t sample = (int32_t)(9000.0f * sinf(phase_a) +
                                   3000.0f * sinf(phase_b));
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        fft_test_input[i] = (int16_t)sample;
        fft_test_buffer[2u * i] = (q15_t)sample;
        fft_test_buffer[(2u * i) + 1u] = 0;
    }

    arm_cfft_q15(&denoiser.fft, fft_test_buffer, 0u, 1u);
    arm_cfft_q15(&denoiser.fft, fft_test_buffer, 1u, 1u);

    uint64_t sum_squares = 0u;
    uint32_t max_error = 0u;
    for (uint32_t i = 0; i < DENOISER_FRAME_SIZE; ++i) {
        int32_t restored = (int32_t)fft_test_buffer[2u * i] *
                           (int32_t)(1u << DENOISER_CFFT_SCALE_BITS);
        if (restored > 32767) restored = 32767;
        if (restored < -32768) restored = -32768;
        const int32_t error = restored - (int32_t)fft_test_input[i];
        const uint32_t magnitude = (uint32_t)((error < 0) ? -error : error);
        if (magnitude > max_error) max_error = magnitude;
        sum_squares += (uint64_t)((int64_t)error * (int64_t)error);
        fft_test_buffer[2u * i] = (q15_t)restored;
    }

    const uint32_t input_rms = rms_q15(fft_test_input, DENOISER_FRAME_SIZE);
    /* Read the corrected real samples from the interleaved diagnostic buffer. */
    uint64_t output_sum_squares = 0u;
    for (uint32_t i = 0; i < DENOISER_FRAME_SIZE; ++i) {
        const int32_t sample = fft_test_buffer[2u * i];
        output_sum_squares += (uint64_t)(sample * sample);
    }
    const uint32_t corrected_rms = (uint32_t)sqrtf(
        (float)output_sum_squares / (float)DENOISER_FRAME_SIZE);
    const uint32_t error_rms = (uint32_t)sqrtf(
        (float)sum_squares / (float)DENOISER_FRAME_SIZE);

    *input_rms_out = input_rms;
    *output_rms_out = corrected_rms;
    *error_rms_out = error_rms;
    *max_error_out = max_error;

    /* Check amplitude and reconstruction sanity, not perceptual quality. */
    return (corrected_rms >= (input_rms * 8u) / 10u) &&
           (corrected_rms <= (input_rms * 12u) / 10u) &&
           (error_rms <= (input_rms * 2u) / 10u);
}

int main(void)
{
    stdio_init_all();
    sleep_ms(1500);

    printf("Pico spectral-subtraction denoiser | CMSIS-DSP Q15\n");
    printf("Target: RP2040 | system clock: %lu MHz\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000u));
    printf("Sample rate: %u Hz | frame: %u | hop: %u | deadline: %u us\n",
           DENOISER_SAMPLE_RATE_HZ,
           DENOISER_FRAME_SIZE,
           DENOISER_HOP_SIZE,
           HOP_BUDGET_US);
    printf("FFT: CMSIS-DSP 512-point Q15 CFFT | inverse scale restoration: x%lu\n",
           (unsigned long)(1u << DENOISER_CFFT_SCALE_BITS));
    printf("DSP gain/noise estimator: integer Q15 / uint32 arithmetic\n\n");

    if (!denoiser_init(&denoiser)) {
        printf("ERROR: CMSIS-DSP Q15 FFT initialization failed.\n");
        while (true) tight_loop_contents();
    }

    uint32_t fft_input_rms = 0u;
    uint32_t fft_output_rms = 0u;
    uint32_t fft_error_rms = 0u;
    uint32_t fft_max_error = 0u;
    const bool fft_scale_ok = fft_roundtrip_diagnostic(
        &fft_input_rms, &fft_output_rms, &fft_error_rms, &fft_max_error);
    printf("FFT round-trip check: %s | input RMS %lu | restored RMS %lu | error RMS %lu | max error %lu\n",
           fft_scale_ok ? "PASS" : "CHECK",
           (unsigned long)fft_input_rms,
           (unsigned long)fft_output_rms,
           (unsigned long)fft_error_rms,
           (unsigned long)fft_max_error);
    if (!fft_scale_ok) {
        printf("ERROR: FFT/IFFT scaling diagnostic is outside the expected range.\n");
        printf("Benchmark stopped; inspect CMSIS-DSP version and Q15 scaling.\n");
        while (true) tight_loop_contents();
    }

    printf("\nSynthetic test: 0.992 s noise-only, then 2.008 s 440 Hz tone + noise\n");
    printf("Timing measures denoiser_process_hop() only; generation/printing excluded.\n\n");

    uint64_t total_process_us = 0u;
    uint32_t min_process_us = UINT32_MAX;
    uint32_t max_process_us = 0u;
    uint32_t measured_hops = 0u;
    uint32_t over_budget_hops = 0u;

    for (uint32_t hop = 0; hop < TEST_HOPS; ++hop) {
        const bool tone_on = (hop >= NOISE_ONLY_HOPS);
        const uint32_t first_sample = hop * DENOISER_HOP_SIZE;

        /* Input generation is deliberately outside the timed interval. */
        for (uint32_t i = 0; i < DENOISER_HOP_SIZE; ++i) {
            input_hop[i] = make_test_sample(first_sample + i, tone_on);
        }

        const uint32_t start_us = time_us_32();
        denoiser_process_hop(&denoiser, input_hop, output_hop);
        const uint32_t elapsed_us = time_us_32() - start_us;

        total_process_us += elapsed_us;
        if (elapsed_us < min_process_us) min_process_us = elapsed_us;
        if (elapsed_us > max_process_us) max_process_us = elapsed_us;
        if (elapsed_us > HOP_BUDGET_US) ++over_budget_hops;
        ++measured_hops;

        if ((hop % 16u) == 15u || hop == TEST_HOPS - 1u) {
            const uint32_t input_rms = rms_q15(input_hop, DENOISER_HOP_SIZE);
            const uint32_t output_rms = rms_q15(output_hop, DENOISER_HOP_SIZE);
            printf("hop %3lu | %-9s | DSP %5lu us | RMS in/out %lu/%lu\n",
                   (unsigned long)hop,
                   tone_on ? "tone+noise" : "noise-only",
                   (unsigned long)elapsed_us,
                   (unsigned long)input_rms,
                   (unsigned long)output_rms);
        }
    }

    const uint32_t average_us = (uint32_t)(total_process_us / measured_hops);
    printf("\nRESULT: average %lu us, best %lu us, worst %lu us, budget %u us\n",
           (unsigned long)average_us,
           (unsigned long)min_process_us,
           (unsigned long)max_process_us,
           HOP_BUDGET_US);
    printf("Hops over budget: %lu / %lu\n",
           (unsigned long)over_budget_hops,
           (unsigned long)measured_hops);
    printf("Timing pass (worst <= budget): %s\n",
           (max_process_us <= HOP_BUDGET_US) ? "YES" : "NO");
    printf("NOTE: Synthetic test only. RMS is not SNR or a speech-quality score.\n");
    printf("Test finished.\n");

    while (true) tight_loop_contents();
}
