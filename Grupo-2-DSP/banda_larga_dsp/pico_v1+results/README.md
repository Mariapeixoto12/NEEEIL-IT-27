# Pico USB denoiser — prova de conceito

Este projeto mantém o **mesmo algoritmo de subtração espectral Q15 com CMSIS-DSP** e acrescenta uma entrada/saída prática através do USB.

- O computador envia áudio PCM mono de 16 kHz e 16 bits.
- A Pico recebe blocos de 256 amostras e devolve blocos processados.
- O computador reconstrói um ficheiro WAV com o resultado.
- Existem modos de diagnóstico para separar problemas de USB, FFT/IFFT e redução de ruído.
- Também existe um comando de benchmark (`B`) que usa um sinal sintético e imprime os tempos de processamento.

**Não é USB Audio Class e não transforma a Pico numa placa de som do sistema operativo.** É uma ligação USB CDC/serial com um protocolo simples de bytes. Para uma prova de conceito, evita precisar de microfone analógico, jack, DAC, amplificador ou osciloscópio.

## O que está e não está implementado

| Parte | Estado |
|---|---|
| Algoritmo de subtração espectral Q15 | Implementado em `denoiser.c` |
| Entrada de áudio pela USB CDC | Implementada |
| Saída de áudio processado pela USB CDC | Implementada |
| Script para enviar WAV e receber WAV | Implementado em `host/pico_host.py` |
| Benchmark sintético com média/mínimo/máximo | Implementado |
| Modo `E` (eco sem DSP) para testar USB | Implementado |
| Modo `U` (FFT/IFFT sem redução de ruído) | Implementado |
| Entrada de microfone pelo ADC | Não implementada nesta versão |
| Saída analógica por DAC/PWM | Não implementada |
| Reprodução em tempo real como dispositivo de áudio USB | Não implementada |

## 1. Compilar para a tua Pico 2

Instala o Raspberry Pi Pico SDK **2.0.0 ou superior** (necessário para RP2350/Pico 2), CMake, Ninja, Git e a ferramenta Arm GNU (`arm-none-eabi-gcc`). Define `PICO_SDK_PATH` para a pasta do Pico SDK.

Dentro da pasta do projeto:

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

O ficheiro a copiar para a Pico 2 é:

```text
build/pico_usb_denoiser.uf2
```

Para compilar para a Pico original (RP2040), usa `-DPICO_BOARD=pico` em vez de `pico2`. O CMake escolhe a definição CMSIS adequada ao processador selecionado pelo SDK. A versão Pico 2 usa o alvo Arm Cortex-M33; a Pico original usa Cortex-M0+.

O CMake descarrega automaticamente versões fixas do CMSIS-DSP (`v1.18.0`) e do CMSIS-Core (`v6.4.0`). É necessária ligação à Internet na primeira configuração, a menos que indiques cópias locais com `CMSIS_DSP_PATH` e `CMSIS_CORE_PATH`.

## 2. Gravar o firmware

1. Desliga a Pico 2 do USB.
2. Mantém premido `BOOTSEL` enquanto a ligas ao computador.
3. Abre a unidade USB que aparece.
4. Copia `build/pico_usb_denoiser.uf2` para essa unidade.
5. A Pico reinicia e aparece como uma porta série USB.

## 3. Testar o benchmark

Instala a dependência do script para PC:

```bash
python -m pip install pyserial
```

Identifica a porta série da Pico (por exemplo, `COM5` no Windows ou `/dev/ttyACM0` em Linux) e executa:

```bash
python host/pico_host.py --port COM5 --benchmark
```

Ao abrir a porta, o script envia `P\n` e espera pela resposta `PONG`. Isto evita depender de uma mensagem `READY` que pode ter sido enviada antes de o script começar a ouvir. Depois, o programa pede à Pico que gere aproximadamente 3 segundos de sinal: primeiro ruído, depois uma sinusóide de 440 Hz misturada com ruído. A Pico mede apenas o tempo de `denoiser_process_hop()` e apresenta a média, o mínimo, o máximo e o número de blocos que ultrapassam 16 ms.

O limite de 16 ms corresponde a 256 amostras a 16 kHz. O benchmark **não** inclui o tempo de transferência USB.

## 4. Testar com um ficheiro de áudio teu

O ficheiro de entrada tem de ser:

- WAV PCM sem compressão;
- mono;
- 16 bits por amostra;
- 16 000 amostras por segundo.

Podes converter um ficheiro qualquer com FFmpeg:

```bash
ffmpeg -i gravacao_original.wav -ac 1 -ar 16000 -c:a pcm_s16le entrada.wav
```

Primeiro, faz os dois testes de diagnóstico:

```bash
# Apenas USB: a Pico devolve exatamente os bytes recebidos.
python host/pico_host.py --port COM5 --input entrada.wav --output eco.wav --echo

# FFT/IFFT e reconstrução, mas sem aplicar ganhos de redução de ruído.
python host/pico_host.py --port COM5 --input entrada.wav --output unity.wav --unity
```

Se `eco.wav` não for igual a `entrada.wav`, o problema está no transporte ou no programa do computador. Se `eco.wav` estiver correto mas `unity.wav` estiver alterado, o problema está na FFT/IFFT, escala ou reconstrução por sobreposição.

Só depois testa a redução de ruído normal:

```bash
python host/pico_host.py --port COM5 --input entrada.wav --output filtrado.wav
```

O script compensa o atraso inicial de um bloco (16 ms) nos modos `unity` e `denoise`, para que o WAV fique alinhado com o original. O modo `echo` não tem esse atraso.

Não precisas de ligar um microfone à Pico: podes gravar áudio no computador ou usar um WAV de teste. A Pico só recebe números PCM através do USB.

## 5. Protocolo USB

A mesma porta USB CDC transporta comandos em texto e áudio binário.

- `P\n`: ping de sincronização; a Pico responde `PONG` e volta a anunciar `READY`.
- `B\n`: inicia o benchmark sintético.
- `S N\n`: redução de ruído normal para `N` blocos.
- `U N\n`: FFT/IFFT e reconstrução com ganho unitário; desativa a redução de ruído.
- `E N\n`: eco dos bytes recebidos; não executa DSP.
- Cada bloco enviado pelo PC contém 256 amostras `int16` little-endian: 512 bytes.
- A Pico devolve exatamente 512 bytes processados por bloco.
- No fim, a Pico escreve uma linha `DONE` com o tempo médio/máximo do DSP.

O script envia um bloco e espera pelo respetivo bloco de saída antes de continuar. Isto evita encher os buffers USB. É um ensaio de processamento de ficheiros, não uma garantia de áudio em tempo real: o ritmo da transferência depende também do computador, do sistema operativo e do USB CDC.

## 6. O que os tempos significam

Durante o modo de ficheiro, a Pico mede apenas o intervalo entre `time_us_32()` imediatamente antes e imediatamente depois de `denoiser_process_hop()`. Portanto:

- `avg_us`: tempo médio da função DSP por bloco;
- `max_us`: pior tempo DSP observado;
- `over_16ms`: número de blocos DSP que ultrapassaram 16 000 µs.

Um `max_us` abaixo de 16 000 µs é um bom sinal para a carga de cálculo, mas ainda não prova um sistema analógico em tempo real. Um sistema real terá ainda aquisição ADC, buffers/DMA, saída de áudio e interrupções.

## 7. Wokwi

O Wokwi já disponibiliza uma placa `board-pi-pico-2` baseada no RP2350 e simula o Cortex-M33. A documentação indica USB CDC/Serial como suportado parcialmente e disponibiliza um Serial Monitor USB. Assim, o teste mais simples no Wokwi é carregar o firmware Pico 2, abrir o Serial Monitor, escrever `B` e confirmar que aparecem as linhas do benchmark.

O teste de transferência de WAV com `pico_host.py` foi pensado para a Pico física ligada ao computador. O Serial Monitor do Wokwi não é uma ferramenta prática para enviar blocos binários PCM arbitrários. Além disso, os tempos de execução no simulador não devem ser apresentados como benchmark de hardware real.

## 8. Limitações importantes

1. O algoritmo é o mesmo, mas esta versão ainda não faz aquisição ADC nem saída analógica.
2. A ligação USB é um transporte série de dados PCM, não USB Audio Class.
3. O envio por blocos permite testar o resultado sonoro, mas não garante que o computador e a Pico mantenham uma cadência de áudio em tempo real.
4. A subtração espectral pode introduzir artefactos musicais e não garante melhoria de inteligibilidade para qualquer ruído ou gravação.
5. A estimativa de ruído precisa de algum tempo de sinal para estabilizar; avalia o resultado com diferentes gravações e não apenas com o benchmark sintético.

## Ficheiros

- `main.c`: comandos USB, receção/envio de áudio e benchmark.
- `denoiser.c`: algoritmo DSP.
- `denoiser.h`: parâmetros e estado do algoritmo.
- `host/pico_host.py`: programa do computador para benchmark ou transferência WAV.
- `CMakeLists.txt`: compilação para RP2040 ou Pico 2 (RP2350 Arm).
- `GUIA_CODIGO.md`: explicação do percurso real de execução, função a função.


## 9. Ordem recomendada para diagnosticar o áudio

Executa os testes pela seguinte ordem e usa sempre a mesma gravação:

1. `--echo`: valida que os bytes atravessam a ligação USB e são gravados corretamente no WAV.
2. `--unity`: valida a FFT, IFFT, janelas e reconstrução por sobreposição sem alterar deliberadamente o espectro.
3. modo normal (sem `--echo` nem `--unity`): testa a estimativa de ruído e a subtração espectral.

Não avalies o algoritmo de redução de ruído enquanto os dois primeiros testes não passarem. O benchmark `--benchmark` mede apenas velocidade; não mede fidelidade nem qualidade sonora.
