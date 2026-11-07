#include "denoiser.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

/* Spectral-subtraction parameters. */
#define MINIMUM_GAIN_Q15        1638u       /* 0.05 */
#define UNITY_GAIN_Q15          32767u
#define POWER_EPSILON           1u
#define OVERSUB_NUM             4u          /* alpha = 4.0 */
#define NOISE_FLOOR_NUM         361u        /* (1 - 0.05)^2 / 4 = 0.225625 */
#define NOISE_FLOOR_DEN         1600u
#define POWER_OLD_NUM           3u          /* 0.60 old */
#define POWER_NEW_NUM           2u          /* 0.40 new */
#define POWER_WEIGHT_DEN        5u
#define GAIN_OLD_Q15            13107u      /* 0.40 */
#define GAIN_NEW_Q15            19661u      /* 0.60 */

static q15_t float_to_q15(float value)
{
    const float scaled = value * 32768.0f;
    if (scaled >= 32767.0f) return (q15_t)32767;
    if (scaled <= -32768.0f) return (q15_t)-32768;
    if (scaled >= 0.0f) return (q15_t)(scaled + 0.5f);
    return (q15_t)(scaled - 0.5f);
}

static int16_t saturate_i16(int32_t value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static uint32_t abs_q15(q15_t value)
{
    const int32_t wide = (int32_t)value;
    return (uint32_t)((wide < 0) ? -wide : wide);
}

static uint32_t magnitude_squared_q15(q15_t real, q15_t imag)
{
    const uint32_t ar = abs_q15(real);
    const uint32_t ai = abs_q15(imag);
    /* Maximum is 32768^2 + 32768^2 = 2^31; uint32_t is sufficient. */
    return (ar * ar) + (ai * ai);
}

static uint32_t smooth_power(uint32_t old_power, uint32_t new_power)
{
    /* Exact integer floor of 0.60*old + 0.40*new, without 32-bit overflow. */
    const uint32_t whole =
        (old_power / POWER_WEIGHT_DEN) * POWER_OLD_NUM +
        (new_power / POWER_WEIGHT_DEN) * POWER_NEW_NUM;
    const uint32_t remainder =
        ((old_power % POWER_WEIGHT_DEN) * POWER_OLD_NUM +
         (new_power % POWER_WEIGHT_DEN) * POWER_NEW_NUM) / POWER_WEIGHT_DEN;
    return whole + remainder;
}

static void initialise_window(Denoiser *d)
{
    for (uint32_t i = 0; i < DENOISER_FRAME_SIZE; ++i) {
        /* Periodic sqrt-Hann. Analysis*synthesis = Hann; 50% OLA sums to 1. */
        const float phase = (2.0f * PI_F * (float)i) /
                            (float)DENOISER_FRAME_SIZE;
        const float hann = 0.5f - 0.5f * cosf(phase);
        const float root_hann = sqrtf(hann > 0.0f ? hann : 0.0f);
        d->window_q15[i] = float_to_q15(root_hann);
    }
}

bool denoiser_init(Denoiser *d)
{
    if (d == NULL) return false;

    memset(d, 0, sizeof(*d));   // inicializa a zeros
    if (arm_cfft_init_512_q15(&d->fft) != ARM_MATH_SUCCESS) {
        d->initialized = false;
        return false;
    }

    initialise_window(d);

    for (uint32_t bin = 0; bin < DENOISER_BIN_COUNT; ++bin) {
        d->current_block_min[bin] = UINT32_MAX;
        d->gain_previous[bin] = (q15_t)UNITY_GAIN_Q15;
        for (uint32_t block = 0; block < DENOISER_NOISE_BLOCKS; ++block) {
            d->block_min_history[block][bin] = UINT32_MAX;
        }
    }

    d->initialized = true;
    return true;
}

static void calculate_power(Denoiser *d)
{
    for (uint32_t bin = 0; bin < DENOISER_BIN_COUNT; ++bin) {
        const uint32_t power = magnitude_squared_q15(
            d->spectrum[2u * bin], d->spectrum[(2u * bin) + 1u]);

        if (!d->have_power) {
            d->smoothed_power[bin] = power;
        } else {
            d->smoothed_power[bin] =
                smooth_power(d->smoothed_power[bin], power);
        }

        if (d->smoothed_power[bin] < d->current_block_min[bin]) {
            d->current_block_min[bin] = d->smoothed_power[bin];
        }
    }
    d->have_power = true;
}

/* ceil((361/1600)*power), so the gain floor activates at the correct boundary. */
static uint32_t minimum_gain_threshold(uint32_t signal_power)
{
    const uint32_t whole =
        (signal_power / NOISE_FLOOR_DEN) * NOISE_FLOOR_NUM;
    const uint32_t remainder = signal_power % NOISE_FLOOR_DEN;
    const uint32_t rounded_remainder =
        (remainder * NOISE_FLOOR_NUM + NOISE_FLOOR_DEN - 1u) /
        NOISE_FLOOR_DEN;
    return whole + rounded_remainder;
}

/*
 * Convert 4*noise/signal to Q15. The caller ensures the ratio is below
 * 0.9025, so the normalized numerator remains within uint32_t.
 */
static q15_t subtraction_ratio_q15(uint32_t noise_power,
                                   uint32_t signal_power)
{
    uint32_t signal_scaled = signal_power;
    uint32_t noise_scaled = noise_power;

    while (signal_scaled > 32767u) {
        signal_scaled = (signal_scaled + 1u) >> 1;
        noise_scaled = (noise_scaled + 1u) >> 1;
    }
    while (signal_scaled < 16384u) {
        signal_scaled <<= 1;
        noise_scaled <<= 1;
    }

    const uint32_t numerator = (noise_scaled * OVERSUB_NUM) << 15;
    uint32_t ratio = numerator / signal_scaled;
    if (ratio > 32767u) ratio = 32767u;
    return (q15_t)ratio;
}

static q15_t calculate_bin_gain(uint32_t noise_power, uint32_t signal_power)
{
    if (signal_power <= POWER_EPSILON) {
        return (q15_t)UNITY_GAIN_Q15;
    }

    /* G = max(1 - sqrt(4*noise/signal), 0.05). */
    if (noise_power >= minimum_gain_threshold(signal_power)) {
        return (q15_t)MINIMUM_GAIN_Q15;
    }

    const q15_t ratio_q15 = subtraction_ratio_q15(noise_power, signal_power);
    q15_t root_q15 = 0;
    if (arm_sqrt_q15(ratio_q15, &root_q15) != ARM_MATH_SUCCESS) {
        return (q15_t)MINIMUM_GAIN_Q15;
    }

    int32_t gain = (int32_t)UNITY_GAIN_Q15 - (int32_t)root_q15;
    if (gain < (int32_t)MINIMUM_GAIN_Q15) gain = (int32_t)MINIMUM_GAIN_Q15;
    if (gain > (int32_t)UNITY_GAIN_Q15) gain = (int32_t)UNITY_GAIN_Q15;
    return (q15_t)gain;
}

static void calculate_gains(Denoiser *d)
{
    for (uint32_t bin = 0; bin < DENOISER_BIN_COUNT; ++bin) {
        uint32_t noise_power = d->current_block_min[bin];

        for (uint32_t block = 0; block < d->valid_history_blocks; ++block) {
            const uint32_t candidate = d->block_min_history[block][bin];
            if (candidate < noise_power) noise_power = candidate;
        }

        d->gain_work[bin] =
            calculate_bin_gain(noise_power, d->smoothed_power[bin]);
    }

    /* Three-bin frequency smoothing; clamp edges rather than wrap DC/Nyquist. */
    for (uint32_t bin = 0; bin < DENOISER_BIN_COUNT; ++bin) {
        const uint32_t below = (bin == 0u) ? 0u : bin - 1u;
        const uint32_t above = (bin + 1u >= DENOISER_BIN_COUNT)
                                   ? DENOISER_BIN_COUNT - 1u
                                   : bin + 1u;
        const uint32_t sum = (uint16_t)d->gain_work[below] +
                             (uint16_t)d->gain_work[bin] +
                             (uint16_t)d->gain_work[above];
        d->gain_frequency_smoothed[bin] = (q15_t)(sum / 3u);
    }

    /* Temporal smoothing: 0.40 previous gain + 0.60 current gain. */
    for (uint32_t bin = 0; bin < DENOISER_BIN_COUNT; ++bin) {
        int32_t gain = d->gain_frequency_smoothed[bin];
        if (d->have_gain) {
            const uint32_t mixed =
                (uint32_t)(uint16_t)d->gain_previous[bin] * GAIN_OLD_Q15 +
                (uint32_t)(uint16_t)gain * GAIN_NEW_Q15 + 16384u;
            gain = (int32_t)(mixed >> 15);
        }
        if (gain < (int32_t)MINIMUM_GAIN_Q15) gain = (int32_t)MINIMUM_GAIN_Q15;
        if (gain > (int32_t)UNITY_GAIN_Q15) gain = (int32_t)UNITY_GAIN_Q15;
        d->gain_previous[bin] = (q15_t)gain;
        d->gain_work[bin] = (q15_t)gain;
    }
    d->have_gain = true;
}

static void apply_symmetric_gains(Denoiser *d)
{
    /* X[N-k] = conj(X[k]); use equal gains on conjugate bins. */
    for (uint32_t bin = 0; bin < DENOISER_FRAME_SIZE; ++bin) {
        const uint32_t positive_bin =
            (bin <= (DENOISER_FRAME_SIZE / 2u))
                ? bin
                : DENOISER_FRAME_SIZE - bin;
        d->gain_per_bin[bin] = d->gain_work[positive_bin];
    }

    arm_cmplx_mult_real_q15(d->spectrum,
                            d->gain_per_bin,
                            d->spectrum_filtered,
                            DENOISER_FRAME_SIZE);
    memcpy(d->spectrum, d->spectrum_filtered, sizeof(d->spectrum));
}

static void finish_noise_block(Denoiser *d)
{
    if (d->frames_in_current_block < DENOISER_FRAMES_PER_BLOCK) return;

    const uint32_t destination = d->history_write_index;
    for (uint32_t bin = 0; bin < DENOISER_BIN_COUNT; ++bin) {
        d->block_min_history[destination][bin] = d->current_block_min[bin];
        d->current_block_min[bin] = UINT32_MAX;
    }

    d->history_write_index =
        (uint8_t)((d->history_write_index + 1u) % DENOISER_NOISE_BLOCKS);
    if (d->valid_history_blocks < DENOISER_NOISE_BLOCKS) {
        ++d->valid_history_blocks;
    }
    d->frames_in_current_block = 0u;
}

void denoiser_process_hop(Denoiser *d,
                          const int16_t input[DENOISER_HOP_SIZE],
                          int16_t output[DENOISER_HOP_SIZE])
{
    if (output == NULL) return;
    if (d == NULL || input == NULL || !d->initialized) {
        memset(output, 0, DENOISER_HOP_SIZE * sizeof(output[0]));
        return;
    }

    /* Slide the 512-sample frame by one 256-sample hop. */
    memmove(d->input_history,
            d->input_history + DENOISER_HOP_SIZE,
            DENOISER_HOP_SIZE * sizeof(d->input_history[0]));
    memcpy(d->input_history + DENOISER_HOP_SIZE,
           input,
           DENOISER_HOP_SIZE * sizeof(input[0]));

    arm_mult_q15(d->input_history,
                 d->window_q15,
                 d->input_windowed,
                 DENOISER_FRAME_SIZE);

    for (uint32_t i = 0; i < DENOISER_FRAME_SIZE; ++i) {
        d->spectrum[2u * i] = d->input_windowed[i];
        d->spectrum[(2u * i) + 1u] = 0;
    }

    arm_cfft_q15(&d->fft, d->spectrum, 0u, 1u);
    calculate_power(d);
    calculate_gains(d);
    apply_symmetric_gains(d);
    arm_cfft_q15(&d->fft, d->spectrum, 1u, 1u);

    /*
     * CMSIS Q15 CFFT/CIFFT uses a 1/512 scaling for this transform length.
     * Restore the net factor after the inverse. Multiplication is used rather
     * than a signed left shift, avoiding undefined behaviour for negatives.
     */
    const int32_t inverse_scale = (int32_t)(1u << DENOISER_CFFT_SCALE_BITS);
    for (uint32_t i = 0; i < DENOISER_FRAME_SIZE; ++i) {
        const int32_t restored =
            (int32_t)d->spectrum[2u * i] * inverse_scale;
        d->time_samples[i] = (q15_t)saturate_i16(restored);
    }

    arm_mult_q15(d->time_samples,
                 d->window_q15,
                 d->synthesis_windowed,
                 DENOISER_FRAME_SIZE);

    for (uint32_t i = 0; i < DENOISER_FRAME_SIZE; ++i) {
        d->overlap[i] += (int32_t)d->synthesis_windowed[i];
    }

    for (uint32_t i = 0; i < DENOISER_HOP_SIZE; ++i) {
        output[i] = saturate_i16(d->overlap[i]);
    }

    memmove(d->overlap,
            d->overlap + DENOISER_HOP_SIZE,
            DENOISER_HOP_SIZE * sizeof(d->overlap[0]));
    memset(d->overlap + DENOISER_HOP_SIZE,
           0,
           DENOISER_HOP_SIZE * sizeof(d->overlap[0]));

    ++d->frames_in_current_block;
    ++d->processed_frames;
    finish_noise_block(d);
}
