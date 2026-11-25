#include <Arduino.h>
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/time.h"

// Entrada analógica
#define PINO_ADC   26  // GP26: pino físico 31
#define CANAL_ADC   0  // ADC0 corresponde ao GP26

// Saída PWM
#define PINO_PWM   15  // GP15: pino físico 20

// 10 us entre amostras -> taxa nominal de 100 kS/s
constexpr int64_t PERIODO_AMOSTRAGEM_US = 10;

// PWM de 9 bits: contador de 0 a 511
constexpr uint16_t PWM_TOP = 511;
constexpr uint16_t PWM_REPOUSO = 256;

uint slice_num;
uint channel;

struct repeating_timer timer;

// Executado por interrupção.
// Evitar Serial, delay ou outras operações demoradas aqui.
bool timer_callback(struct repeating_timer *t) {
    // ADC de 12 bits: 0 a 4095
    const uint16_t amostra = adc_read();

    // Converte para 9 bits: 0 a 511
    const uint16_t nivel_pwm = amostra >> 3;

    pwm_set_chan_level(slice_num, channel, nivel_pwm);

    return true;
}

void setup() {
    Serial.begin(115200);

    // Configuração do ADC0 no GP26
    adc_init();
    adc_gpio_init(PINO_ADC);
    adc_select_input(CANAL_ADC);

    // Identifica o periférico PWM associado ao GP15
    gpio_set_function(PINO_PWM, GPIO_FUNC_PWM);
    slice_num = pwm_gpio_to_slice_num(PINO_PWM);
    channel = pwm_gpio_to_channel(PINO_PWM);

    // Configuração explícita do PWM
    pwm_config config = pwm_get_default_config();

    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_config_set_phase_correct(&config, false);
    pwm_config_set_wrap(&config, PWM_TOP);

    // Inicializa sem arrancar, define o repouso e depois ativa
    pwm_init(slice_num, &config, false);
    pwm_set_chan_level(slice_num, channel, PWM_REPOUSO);
    pwm_set_enabled(slice_num, true);

    // Frequência PWM calculada a partir do relógio real
    const uint32_t frequencia_sistema = clock_get_hz(clk_sys);
    const float frequencia_pwm =
        static_cast<float>(frequencia_sistema) / (PWM_TOP + 1);

    Serial.print("Relogio do sistema: ");
    Serial.print(frequencia_sistema);
    Serial.println(" Hz");

    Serial.print("Frequencia PWM: ");
    Serial.print(frequencia_pwm, 1);
    Serial.println(" Hz");

    Serial.print("Taxa nominal de amostragem: ");
    Serial.print(1000000UL / PERIODO_AMOSTRAGEM_US);
    Serial.println(" amostras/s");

    // Valor negativo: agenda intervalos entre inícios dos callbacks
    const bool timer_criado = add_repeating_timer_us(
        -PERIODO_AMOSTRAGEM_US,
        timer_callback,
        nullptr,
        &timer
    );

    if (!timer_criado) {
        Serial.println("ERRO: nao foi possivel criar o temporizador.");

        // Mantém a saída no ponto médio se a aquisição não arrancar
        pwm_set_chan_level(slice_num, channel, PWM_REPOUSO);
    }
}

void loop() {
    // A aquisição e atualização do PWM decorrem nas interrupções.
}