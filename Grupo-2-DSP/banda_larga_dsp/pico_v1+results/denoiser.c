#include "denoiser.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

/* Parâmetros da subtração espectral. */
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
    /* O máximo é 2^31; uint32_t chega para guardar o resultado. */
    return (ar * ar) + (ai * ai);
}

static uint32_t smooth_power(uint32_t old_power, uint32_t new_power)
{
    /* Calcula 60% do valor anterior e 40% do novo sem overflow de 32 bits. */
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
        /* Janela sqrt-Hann. Na análise e síntese, a sobreposição a 50% recompõe o sinal. */
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

    memset(d, 0, sizeof(*d));
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

/* Arredonda o limiar para cima para ativar o ganho mínimo no ponto correto. */
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
 * Converte 4*ruido/sinal para Q15. A função que chama garante que a razão
 * é inferior a 0,9025, mantendo o numerador dentro de uint32_t.
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

    /* G = max(1 - sqrt(4*ruido/sinal), 0,05). */
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

    /* Suaviza cada ganho com os dois bins vizinhos; não faz wrap nas extremidades. */
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

    /* Suavização temporal: 40% do ganho anterior e 60% do ganho atual. */
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
    /* As frequências simétricas usam o mesmo ganho para manter o sinal real. */
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

static void process_hop(Denoiser *d,
                        const int16_t input[DENOISER_HOP_SIZE],
                        int16_t output[DENOISER_HOP_SIZE],
                        bool apply_noise_reduction)
{
    if (output == NULL) return;
    if (d == NULL || input == NULL || !d->initialized) {
        memset(output, 0, DENOISER_HOP_SIZE * sizeof(output[0]));
        return;
    }

    /* Desloca a janela de 512 amostras em 256 amostras. */
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

    if (apply_noise_reduction) {
        calculate_power(d);
        calculate_gains(d);
        apply_symmetric_gains(d);
    }

    arm_cfft_q15(&d->fft, d->spectrum, 1u, 1u);

    /*
     * A FFT/IFFT Q15 da CMSIS aplica uma escala de 1/512 neste tamanho.
     * Recuperamos esse fator depois da IFFT. Usamos multiplicação, não
     * deslocamento de bits, para evitar comportamento indefinido em negativos.
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


void denoiser_process_hop(Denoiser *d,
                          const int16_t input[DENOISER_HOP_SIZE],
                          int16_t output[DENOISER_HOP_SIZE])
{
    process_hop(d, input, output, true);
}

void denoiser_process_hop_unity(Denoiser *d,
                                const int16_t input[DENOISER_HOP_SIZE],
                                int16_t output[DENOISER_HOP_SIZE])
{
    process_hop(d, input, output, false);
}
