#ifndef DENOISER_H
#define DENOISER_H

#include <stdbool.h>
#include <stdint.h>
#include "arm_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DENOISER_SAMPLE_RATE_HZ    16000u
#define DENOISER_FRAME_SIZE        512u
#define DENOISER_HOP_SIZE          256u
#define DENOISER_BIN_COUNT         ((DENOISER_FRAME_SIZE / 2u) + 1u)
#define DENOISER_NOISE_BLOCKS      5u
#define DENOISER_FRAMES_PER_BLOCK  16u
#define DENOISER_CFFT_SCALE_BITS   9u

typedef struct {
    arm_cfft_instance_q15 fft;

    q15_t spectrum[2u * DENOISER_FRAME_SIZE];
    q15_t spectrum_filtered[2u * DENOISER_FRAME_SIZE];
    q15_t gain_per_bin[DENOISER_FRAME_SIZE];

    q15_t input_windowed[DENOISER_FRAME_SIZE];
    q15_t time_samples[DENOISER_FRAME_SIZE];
    q15_t synthesis_windowed[DENOISER_FRAME_SIZE];
    q15_t window_q15[DENOISER_FRAME_SIZE];
    int16_t input_history[DENOISER_FRAME_SIZE];
    int32_t overlap[DENOISER_FRAME_SIZE];

    uint32_t smoothed_power[DENOISER_BIN_COUNT];
    uint32_t current_block_min[DENOISER_BIN_COUNT];
    uint32_t block_min_history[DENOISER_NOISE_BLOCKS][DENOISER_BIN_COUNT];

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

/* Call once before processing. Keep this object in static/global memory. */
bool denoiser_init(Denoiser *denoiser);

/* Process one 16 ms block: 256 mono int16 samples in, 256 out.
   The first output block is startup latency and is expected to be mostly zero. */
void denoiser_process_hop(Denoiser *denoiser,
                          const int16_t input[DENOISER_HOP_SIZE],
                          int16_t output[DENOISER_HOP_SIZE]);

/* Diagnostic: run FFT/IFFT and overlap-add, but do not apply noise reduction. */
void denoiser_process_hop_unity(Denoiser *denoiser,
                                const int16_t input[DENOISER_HOP_SIZE],
                                int16_t output[DENOISER_HOP_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
