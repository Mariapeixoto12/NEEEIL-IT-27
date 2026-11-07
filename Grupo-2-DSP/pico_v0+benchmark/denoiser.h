#ifndef DENOISER_H
#define DENOISER_H

#include <stdbool.h>
#include <stdint.h>
#include "arm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed configuration for 16 kHz mono audio on the RP2040. */
#define DENOISER_SAMPLE_RATE_HZ    16000u
#define DENOISER_FRAME_SIZE        512u
#define DENOISER_HOP_SIZE          256u
#define DENOISER_BIN_COUNT         ((DENOISER_FRAME_SIZE / 2u) + 1u)
#define DENOISER_NOISE_BLOCKS      5u
#define DENOISER_FRAMES_PER_BLOCK  16u

/*
 * CMSIS-DSP's Q15 512-point CFFT scales the forward transform by 1/512 to
 * keep the result in Q15 range. The Q15 inverse is also normalized. Therefore
 * a forward+inverse round trip is smaller than the original by 512. Restore
 * those 9 bits after the inverse transform, before synthesis windowing.
 */
#define DENOISER_CFFT_SCALE_BITS   9u

/*
 * Denoiser state. Allocate one instance statically/globally; do not put it on
 * the small RP2040 main stack. Approximate storage is under 20 kB.
 */
typedef struct {
    arm_cfft_instance_q15 fft;

    /* Interleaved complex arrays: Re[0], Im[0], Re[1], Im[1], ... */
    q15_t spectrum[2u * DENOISER_FRAME_SIZE];
    q15_t spectrum_filtered[2u * DENOISER_FRAME_SIZE];
    q15_t gain_per_bin[DENOISER_FRAME_SIZE];

    /* STFT and overlap-add state. */
    q15_t input_windowed[DENOISER_FRAME_SIZE];
    q15_t time_samples[DENOISER_FRAME_SIZE];
    q15_t synthesis_windowed[DENOISER_FRAME_SIZE];
    q15_t window_q15[DENOISER_FRAME_SIZE];
    int16_t input_history[DENOISER_FRAME_SIZE];
    int32_t overlap[DENOISER_FRAME_SIZE];

    /* Per-bin power is unsigned Q-format magnitude-squared (common scale). */
    uint32_t smoothed_power[DENOISER_BIN_COUNT];
    uint32_t current_block_min[DENOISER_BIN_COUNT];
    uint32_t block_min_history[DENOISER_NOISE_BLOCKS][DENOISER_BIN_COUNT];

    /* Gains are Q15 in [0, 32767]. */
    q15_t gain_previous[DENOISER_BIN_COUNT];
    q15_t gain_work[DENOISER_BIN_COUNT];
    q15_t gain_frequency_smoothed[DENOISER_BIN_COUNT];

    uint32_t processed_frames;
    uint8_t frames_in_current_block;
    uint8_t history_write_index;
    uint8_t valid_history_blocks;
    bool have_power;
    bool have_gain;
    bool initialized;
} Denoiser;

/* Initialize tables, window, gains and noise-estimator state. */
bool denoiser_init(Denoiser *denoiser);

/*
 * Process exactly 256 mono signed-16-bit PCM samples and write 256 samples.
 * Call once per 16 ms hop at 16 kHz. The first output hop is startup latency
 * and is expected to be mostly zero due to the 50% overlapped frame history.
 */
void denoiser_process_hop(Denoiser *denoiser,
                          const int16_t input[DENOISER_HOP_SIZE],
                          int16_t output[DENOISER_HOP_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* DENOISER_H */
