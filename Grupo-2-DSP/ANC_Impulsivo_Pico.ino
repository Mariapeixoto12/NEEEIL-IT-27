#include <Arduino.h>
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "pico/time.h"

// Escolher algoritmo:
// 0 = Sem filtro
// 1 = Median Filter
// 2 = Clipping
// 3 = Detecao + Interpolacao

#define MODO 1 // alterem aqui o modo 

// HARDWARE
#define PINO_ADC 27
#define CANAL_ADC 1
#define PINO_PWM 15

// PARAMETROS
#define PERIODO_US 20
#define ADC_CENTRO 2048

#define LIMIAR_CLIP 1200
#define LIMIAR_PICO 450
#define VIZINHOS_MAX_DIFF 350

#if MODO < 0 || MODO > 3
#error "Escolhe MODO entre 0 e 3"
#endif

uint32_t slice_num;
uint32_t channel;
struct repeating_timer timer_audio;

// Historico de amostras
uint16_t mais_antiga = ADC_CENTRO;
uint16_t anterior = ADC_CENTRO;

volatile uint32_t amostras = 0;
volatile uint32_t picos_corrigidos = 0;


// FUNCOES AUXILIARES
int32_t modulo32(int32_t v) {return (v < 0) ? -v : v};



// METODO 1 - MEDIAN FILTER
uint16_t mediana3(uint16_t a, uint16_t b, uint16_t c) 
{
    uint16_t temp;

    if (a > b) 
    {
        temp = a;
        a = b;
        b = temp;
    }

    if (b > c) 
    {
        temp = b;
        b = c;
        c = temp;
    }

    if (a > b) 
    {
        temp = a;
        a = b;
        b = temp;
    }

    return b;
}


// METODO 2 - CLIPPING
uint16_t clipping(uint16_t entrada) 
{
    int32_t v = (int32_t)entrada - ADC_CENTRO;

    if (v > LIMIAR_CLIP) v = LIMIAR_CLIP;

    if (v < -LIMIAR_CLIP) v = -LIMIAR_CLIP;

    int32_t saida = v + ADC_CENTRO;

    if (saida < 0) saida = 0;
    if (saida > 4095) saida = 4095;

    return (uint16_t)saida;
}

// METODO 3 - DETECAO + INTERPOLACAO
uint16_t declick(uint16_t a, uint16_t b, uint16_t c, bool &corrigiu) 
{
    int32_t previsto = ((int32_t)a + (int32_t)c) / 2; // estimativa do valor central

    int32_t desvio = modulo32((int32_t)b - previsto); // diferença entre o real e estimado

    int32_t diferenca_vizinhos = modulo32((int32_t)a - (int32_t)c); // comparar os dois vizinhos
  
    corrigiu = (desvio > LIMIAR_PICO) && (diferenca_vizinhos < VIZINHOS_MAX_DIFF); // existe pico isolado?

    if (corrigiu) return (uint16_t)previsto; // foi corrigido!

    return b;
}

// ===========================================
// ESCOLHER O METODO
// ===========================================

uint16_t processar(uint16_t a, uint16_t b, uint16_t c) 
{

  if(MODO == 0) {return b};
  else if(MODO == 1) {return mediana3(a, b, c)};
  else if (MODO == 2) {return clipping(b)};
  else {bool corrigiu = false};

  uint16_t resultado = declick(a, b, c, corrigiu);

  if (corrigiu) picos_corrigidos++; // corrigimos mais um pico

  return resultado;
}


// INTERRUPCAO - 50 kHz
bool timer_callback(struct repeating_timer *t) 
{
  (void)t;

  // 1. Ler o microfone
  uint16_t nova = adc_read();

  // 2. Processar a amostra central
  uint16_t filtrada = processar(mais_antiga, anterior, nova);

  // 3. Atualizar historico
  mais_antiga = anterior;
  anterior = nova;

  // 4. Converter ADC 12 bits para PWM 10 bits
  pwm_set_chan_level(slice_num, channel, filtrada >> 2);

  amostras++;

  return true;
}


// SETUP
void setup() 
{
  Serial.begin(115200);

  // ADC
  adc_init();
  adc_gpio_init(PINO_ADC);
  adc_select_input(CANAL_ADC);

  // PWM
  gpio_set_function(PINO_PWM, GPIO_FUNC_PWM);

  slice_num = pwm_gpio_to_slice_num(PINO_PWM);
  channel = pwm_gpio_to_channel(PINO_PWM);

  pwm_set_wrap(slice_num, 1023);

  pwm_set_chan_level(slice_num, channel, ADC_CENTRO >> 2);

  pwm_set_enabled(slice_num, true);

  // Temporizador de 20 microssegundos
  bool ok = add_repeating_timer_us(-PERIODO_US, timer_callback, nullptr, &timer_audio);

  if (!ok) Serial.println("ERRO: temporizador");
}

// LOOP
void loop() 
{
  delay(1000);

  Serial.print("Modo: ");
  Serial.print(MODO);

  Serial.print(" | Amostras: ");
  Serial.print(amostras);

  if(MODO == 3) {Serial.print(" | Picos corrigidos: ");}
  Serial.print(picos_corrigidos);

  Serial.println();
}
