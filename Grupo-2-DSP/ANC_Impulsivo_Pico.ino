/*
  NEEEIL-IT | ANC para ruido impulsivo | Arduino IDE / Raspberry Pi Pico RP2040
  Tres modos: 0 FxLMS normal; 1 FxLMS com clipping; 2 FxLMS com thresholding.
  microfone de referencia GP26 (ADC0), microfone de erro GP27 (ADC1),
  GP15 PWM -> filtro RC passa-baixo e amplificador de audio externo.
*/

#include <Arduino.h>
#include "hardware/adc.h" // permite controlar o conversor analog-dig da pico
#include "hardware/pwm.h" // configura saida pwm
#include "pico/time.h" // temporizador periodico para amostrar os microfones
#include <math.h> 
#include <stdint.h> // tipos inteiros

class ImpulsiveFxLMS 
{ // esta classe vai reunir o que e necessario para o fxlms funcionar
public:
  enum Mode : uint8_t { BASELINE = 0, CLIPPING = 1, THRESHOLDING = 2 }; // os 3 codigos 
  static constexpr int W = 16; // filtro adaptativo usa 16 coeficientes
  static constexpr int S = 8; // percurso secundario (estimativa) usa 8 coeficientes

  explicit ImpulsiveFxLMS(Mode m = BASELINE) : mode(m) { reset(); } // define o modo (1-3) e usa reset (ver prox linhas)

  void reset() // coloca todos os coeficientes a zero, para o programa estar ok para começar
  {
    for (int i = 0; i < W; i++) 
    { 
      weights[i]=0; 
      x_history[i]=0; 
      xf_history[i]=0; 
    }

    for (int i = 0; i < S; i++) clipped_history[i]=0;

    last_output=0; // ultima saida calculada é 0
    last_frozen=false; // nao existe, nesse momento, uma suspensao de adaptação assinalada
  }

  void setMode(Mode m) { mode = m; }
  void setSecondaryEstimate(const float (&coeff)[S]) // recebe os coef da estimativa do percurso sec e guarda-os num array
  {
    for (int i=0; i<S; i++) secondary[i]=coeff[i];

    model_ready = true; // foi fornecido um modelo
  }
  bool isReady() const { return model_ready; }

  // MU, limites e amplitude sao parametros de experiencia, nao valores calibrados.
  float mu = 0.0004f; // step size
  float error_threshold = 0.35f; // limiar do erro para limitar erros elevados
  float reference_threshold = 0.35f; // limitar amplitudes elevadas da ref
  float output_limit = 0.25f; // amplitude max do sinal de saida
  float last_output = 0; // guarda a ultima saida calculada
  bool last_frozen = false; // a adaptação foi suspensa? 

  // UMA vez por amostra, depois de adquirir referencia e microfone de erro.
  float process(float x, float e) { // x é o sinal captado pelo micro de ref; e é o erro captado pelo micro de erro
    if (!model_ready) { last_output = 0; return 0; }
    x = clamp(x, -1.0f, 1.0f); // clamp garante que x esta no intervalo pretendido
    e = clamp(e, -1.0f, 1.0f); // linhas 57-60 é verificação e normalização

    push(x_history, W, x); // coloca a nova amostra de ref no historico, deslocando anteriores
    float raw_output=0;
    for (int i=0; i<W; i++) raw_output -= weights[i] * x_history[i]; // sinal de controlo gerado pelo filtro
    const float output = clamp(raw_output, -output_limit, output_limit);
    last_output = output;

    // clipping
    const float x_grad = (mode == CLIPPING) ?
      clamp(x, -reference_threshold, reference_threshold) : x;

    push(clipped_history, S, x_grad);
    float xf = 0; // amostra da ref filtrada pela estimativa do percurso sec
    for(int i=0; i<S; i++) xf += secondary[i] * clipped_history[i];
    push(xf_history, W, xf); 

    // thresholding; fabsf é valor absoluto de um float
    last_frozen = false;
    if (mode == THRESHOLDING &&
        (fabsf(e) > error_threshold || fabsf(x) > reference_threshold)) {
      last_frozen = true; // suspende-se atualização
      return output;
    }

    // clipping do erro
    float robust_error = (mode == CLIPPING) ?
      clamp(e, -error_threshold, error_threshold) : e;

    // proteção adicional perante saturação; aplicada aos 3 modos
    if (fabsf(raw_output) > output_limit) {
      last_frozen = true;
      return output;
    }

    // atualização dos coeficientes
    for (int i = 0; i < W; i++) weights[i] += mu * robust_error * xf_history[i];
    return output; // antirruido calculado
  }

  float coefficientNorm() const { // norma dos coeficientes
    float s = 0;
    for(int i = 0; i < W; i++) s += weights[i] * weights[i];
    return sqrtf(s);
  } 

private:
  Mode mode;
  bool model_ready = false;

  float weights[W] = {}; // coef adaptativos do fxlms
  float x_history[W] = {}; // historico da ref original
  float xf_history[W] = {}; // historico da ref filtrada
  float clipped_history[S] = {}; // historico da ref usada na adaptação
  float secondary[S] = {}; // coef estimados do percurso sec
  static float clamp(float v, float a, float b) {return v < a ? a : (v > b ? b : v);}
  static void push(float *v, int n, float sample) 
  {
    for(int i = n-1; i>0; i--) v[i] = v[i-1];
    v[0] = sample;
  }
};




// Selecionar 0, 1 ou 2 e recompilar para comparar as tres versoes.
#define ANC_MODE 1
// 0 = FxLMS convencional
// 1 = FxLMS + clipping (erro e referencia para adaptacao)
// 2 = FxLMS + thresholding (suspende adaptacao acima dos limites)

#define GP_REF 26    // ADC0 - microfone referencia
#define GP_ERROR 27  // ADC1 - microfone erro
#define GP_PWM 15

static constexpr int SAMPLE_PERIOD_US = 100; // 10 kHz, validar carga real
ImpulsiveFxLMS anc((ImpulsiveFxLMS::Mode)ANC_MODE); // objeto anc que executará o algoritmo selecionado
repeating_timer timer_anc; // giarda o temporizador periódico
uint pwm_slice, pwm_channel; // bloco PWM e canal PWM, respetivamente
volatile uint32_t samples = 0; // quantas amostras foram processadas 

float adcAudio(uint8_t channel) 
{
  adc_select_input(channel); // seleciona o canal
  uint16_t sample = adc_read(); // le um valor inteiro de 12 bits
  return ((float)sample - 2048.0f) / 2048.0f; // valor entre -1 e 1
}

// executar uma amostra:
bool sampleCallback(repeating_timer *timer) {
  // o RP2040 tem um so ADC multiplexado: as leituras sao sequenciais.
  const float x = adcAudio(0); 
  const float e = adcAudio(1);
  const float y = anc.process(x,e); // funcao process() que executa o fxlms escolhido

  int level = (int)(512.0f + 511.0f * y);
  if(level < 0) level = 0;
  if(level > 1023) level = 1023; // transformam o sinal de saida num pwm de 10 bits apropriado
 
  pwm_set_chan_level(pwm_slice, pwm_channel, (uint16_t)level); // envia o valor calculado para canal PWM

  ++samples; // next
  return true;
}

void setup() {
  Serial.begin(115200);

  adc_init(); // inicializa o ADC
  adc_gpio_init(GP_REF); // prepara os pinos analog
  adc_gpio_init(GP_ERROR); // o mm de cima 

  gpio_set_function(GP_PWM, GPIO_FUNC_PWM); // é saída PWM
  pwm_slice = pwm_gpio_to_slice_num(GP_PWM); // bloco
  pwm_channel = pwm_gpio_to_channel(GP_PWM); // canal

  pwm_set_wrap(pwm_slice, 1023); // PWM de 0 a 1023
  pwm_set_chan_level(pwm_slice, pwm_channel, 512); // a saida esta inicialmente a meio
  pwm_set_enabled(pwm_slice, true); // ativa o PWM

  if (!anc.isReady()) 
  {
    Serial.println("ANC bloqueado: falta calibrar S_EST. Saida em silencio DC.");
    // por seguranca NAO inicializa timer ate calibrar modelo.
    return;
  }

  if (!add_repeating_timer_us(-SAMPLE_PERIOD_US, sampleCallback, nullptr, &timer_anc)) Serial.println("Falha ao iniciar temporizador");
}

void loop() {
  delay(1000);
  Serial.print("Amostras: "); 
  Serial.println(samples);
}
