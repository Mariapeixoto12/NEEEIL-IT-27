#include <Arduino.h>
#include "hardware/adc.h"
#include "hardware/pwm.h"

// --- MAPEAMENTO DOS PINOS ---
#define PINO_ADC    27      // GP27 (Pino físico 32 da Pico)
#define CANAL_ADC   0       // ADC0 (Canal associado ao GP26)
#define PINO_PWM    15      // GP15 (Pino físico 20 da Pico)

uint slice_num;
uint channel;
struct repeating_timer timer;

// Interrupção de hardware executada rigorosamente a 50 kHz (a cada 20 µs)
bool timer_callback(struct repeating_timer *t) {
    // 1. Lê a tensão analógica do ADC (12 bits: 0 a 4095)
    uint16_t amostra = adc_read();

    // 2. Mapeamento linear 1:1 para a gama do PWM (10 bits: 0 a 1023)
    // Divisão inteira por 4 (shift de 2 bits) em 1 ciclo de clock
    pwm_set_chan_level(slice_num, channel, amostra >> 2);

    return true;
}

void setup() {
    // 1. Inicialização do ADC
    adc_init();
    adc_gpio_init(PINO_ADC);
    adc_select_input(CANAL_ADC);

    // 2. Configuração do PWM de alta velocidade
    gpio_set_function(PINO_PWM, GPIO_FUNC_PWM);
    slice_num = pwm_gpio_to_slice_num(PINO_PWM);
    channel = pwm_gpio_to_channel(PINO_PWM);

    // wrap = 1023 -> Resolução de 10 bits
    // Frequência portadora = 125 MHz / 1024 ≈ 122.07 kHz
    pwm_set_wrap(slice_num, 1023);
    pwm_set_chan_level(slice_num, channel, 512); // Ponto de repouso (1.65V)
    pwm_set_enabled(slice_num, true);

    // 3. Ativa o temporizador periódico de amostragem por interrupção (-20 µs = 50 kHz)
    add_repeating_timer_us(-20, timer_callback, NULL, &timer);
}

void loop() {
    // O loop fica completamente livre; a transferência corre por hardware em background
}