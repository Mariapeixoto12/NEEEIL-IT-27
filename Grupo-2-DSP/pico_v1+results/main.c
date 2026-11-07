#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/clocks.h"
#include "denoiser.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HOP_BUDGET_US 16000u
#define TEST_HOPS 188u
#define NOISE_ONLY_HOPS 62u
#define MAX_STREAM_HOPS 200000u
#define PI_F 3.14159265358979323846f

static Denoiser denoiser;
static int16_t input_hop[DENOISER_HOP_SIZE];
static int16_t output_hop[DENOISER_HOP_SIZE];
static uint32_t random_state = 0x12345678u;

/* Lê uma linha de comando. Depois do comando, os dados de áudio são binários. */
static bool read_line(char *line, size_t capacity)
{
    size_t used = 0;

    while (used + 1u < capacity) {
        int value = getchar_timeout_us(1000000u);
        if (value < 0) continue;
        if (value == '\r') continue;
        if (value == '\n') {
            line[used] = '\0';
            return true;
        }
        line[used++] = (char)value;
    }

    line[used] = '\0';
    return false;
}

/* Lê um bloco completo, mesmo que o USB o entregue em vários pacotes. */
static bool usb_read_exact(uint8_t *buffer, uint32_t size)
{
    uint32_t received = 0;
    absolute_time_t deadline = make_timeout_time_ms(5000u);

    while (received < size) {
        int count = stdio_get_until((char *)buffer + received,
                                    (int)(size - received), deadline);
        if (count <= 0) return false;
        received += (uint32_t)count;
    }
    return true;
}

/* Envia bytes sem conversão: o áudio pode conter qualquer valor de byte. */
static bool usb_write_exact(const uint8_t *buffer, uint32_t size)
{
    int written = stdio_put_string((const char *)buffer, (int)size, false, false);
    stdio_flush();
    return written == (int)size;
}

static uint32_t random_u32(void)
{
    uint32_t x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    random_state = x;
    return x;
}

/* Gera sempre o mesmo teste: primeiro ruído; depois tom de 440 Hz mais ruído. */
static int16_t make_test_sample(uint32_t sample_index, bool tone_enabled)
{
    int32_t random_value = (int32_t)(random_u32() & 0xFFFFu) - 32768;
    int32_t noise = (random_value * 1200) / 32768;
    int32_t tone = 0;

    if (tone_enabled) {
        float phase = 2.0f * PI_F * 440.0f * (float)sample_index /
                      (float)DENOISER_SAMPLE_RATE_HZ;
        tone = (int32_t)(7000.0f * sinf(phase));
    }

    int32_t sample = tone + noise;
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    return (int16_t)sample;
}

/* Mede a mesma função DSP usando um sinal gerado dentro da Pico. */
static void run_benchmark(void)
{
    if (!denoiser_init(&denoiser)) {
        printf("ERROR: could not initialise the denoiser\n");
        return;
    }

    uint64_t total_us = 0;
    uint32_t best_us = UINT32_MAX;
    uint32_t worst_us = 0;
    uint32_t over_budget = 0;

    for (uint32_t hop = 0; hop < TEST_HOPS; ++hop) {
        bool tone_enabled = (hop >= NOISE_ONLY_HOPS);
        uint32_t first_sample = hop * DENOISER_HOP_SIZE;

        for (uint32_t i = 0; i < DENOISER_HOP_SIZE; ++i) {
            input_hop[i] = make_test_sample(first_sample + i, tone_enabled);
        }

        uint32_t start = time_us_32();
        denoiser_process_hop(&denoiser, input_hop, output_hop);
        uint32_t elapsed = time_us_32() - start;

        total_us += elapsed;
        if (elapsed < best_us) best_us = elapsed;
        if (elapsed > worst_us) worst_us = elapsed;
        if (elapsed > HOP_BUDGET_US) ++over_budget;
    }

    printf("BENCHMARK: %u hops, %u Hz sample rate\n",
           TEST_HOPS, DENOISER_SAMPLE_RATE_HZ);
    printf("DSP time: average=%lu us, best=%lu us, worst=%lu us\n",
           (unsigned long)(total_us / TEST_HOPS),
           (unsigned long)best_us,
           (unsigned long)worst_us);
    printf("16 ms deadline: %s (%lu hops over budget)\n",
           (worst_us <= HOP_BUDGET_US) ? "PASS" : "FAIL",
           (unsigned long)over_budget);
    printf("Note: this times the DSP function only, not USB transfer time.\n");
    printf("BENCHMARK_DONE\n");
}

typedef enum {
    STREAM_DENOISE,
    STREAM_UNITY,
    STREAM_ECHO
} StreamMode;

/* Receives PCM blocks and returns raw, unity-processed or denoised audio. */
static void run_usb_stream(uint32_t hop_count, StreamMode mode)
{
    if (mode != STREAM_ECHO && !denoiser_init(&denoiser)) {
        printf("ERROR: could not initialise the denoiser\n");
        return;
    }

    const char *mode_name = (mode == STREAM_ECHO) ? "echo" :
                            (mode == STREAM_UNITY) ? "unity" : "denoise";
    printf("START mode=%s\n", mode_name);
    stdio_flush();

    uint64_t total_us = 0;
    uint32_t worst_us = 0;
    uint32_t over_budget = 0;

    for (uint32_t hop = 0; hop < hop_count; ++hop) {
        if (!usb_read_exact((uint8_t *)input_hop, sizeof(input_hop))) {
            printf("ERROR: USB input timeout at hop %lu\n", (unsigned long)hop);
            return;
        }

        if (mode == STREAM_ECHO) {
            memcpy(output_hop, input_hop, sizeof(output_hop));
        } else {
            uint32_t start = time_us_32();
            if (mode == STREAM_UNITY) {
                denoiser_process_hop_unity(&denoiser, input_hop, output_hop);
            } else {
                denoiser_process_hop(&denoiser, input_hop, output_hop);
            }
            uint32_t elapsed = time_us_32() - start;

            total_us += elapsed;
            if (elapsed > worst_us) worst_us = elapsed;
            if (elapsed > HOP_BUDGET_US) ++over_budget;
        }

        if (!usb_write_exact((const uint8_t *)output_hop, sizeof(output_hop))) {
            printf("ERROR: USB output failed at hop %lu\n", (unsigned long)hop);
            return;
        }
    }

    printf("DONE mode=%s hops=%lu avg_us=%lu max_us=%lu over_16ms=%lu\n",
           mode_name,
           (unsigned long)hop_count,
           (unsigned long)(total_us / hop_count),
           (unsigned long)worst_us,
           (unsigned long)over_budget);
}

int main(void)
{
    stdio_init_all();

    /* Só anuncia READY depois de o computador abrir a porta USB. */
    while (!stdio_usb_connected()) tight_loop_contents();
    sleep_ms(50u);

    if (!denoiser_init(&denoiser)) {
        printf("ERROR: CMSIS-DSP FFT initialisation failed\n");
        while (true) tight_loop_contents();
    }

    printf("Pico USB denoiser | %lu MHz | %u Hz | frame %u | hop %u\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
           DENOISER_SAMPLE_RATE_HZ,
           DENOISER_FRAME_SIZE,
           DENOISER_HOP_SIZE);
    printf("Commands: P = ping; B = benchmark; S = denoise; U = unity test; E = USB echo\n");

    while (true) {
        char command[48];
        printf("READY\n");
        stdio_flush();

        if (!read_line(command, sizeof(command))) {
            printf("ERROR: command too long\n");
            continue;
        }

        if (strcmp(command, "P") == 0) {
            printf("PONG\n");
        } else if (strcmp(command, "B") == 0) {
            run_benchmark();
        } else {
            char mode_letter = 0;
            unsigned long hops = 0;
            if (sscanf(command, "%c %lu", &mode_letter, &hops) == 2 &&
                hops > 0u && hops <= MAX_STREAM_HOPS &&
                (mode_letter == 'S' || mode_letter == 'U' || mode_letter == 'E')) {
                StreamMode mode = (mode_letter == 'E') ? STREAM_ECHO :
                                  (mode_letter == 'U') ? STREAM_UNITY :
                                                        STREAM_DENOISE;
                run_usb_stream((uint32_t)hops, mode);
            } else {
                printf("Unknown command. Use P, B, S <hops>, U <hops> or E <hops>.\n");
            }
        }
    }
}
