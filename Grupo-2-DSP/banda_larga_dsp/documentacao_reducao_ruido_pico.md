# Redução de ruído em áudio com Raspberry Pi Pico

**Estado do projeto em 7 de novembro de 2026**  
Documento de apoio à reunião técnica — linguagem simples, mas com detalhe suficiente para explicar as decisões de engenharia.

> **Mensagem principal:** já existe uma prova de conceito que corre na Pico 2 W, recebe e devolve áudio por USB e executa o algoritmo com bastante margem de tempo. No entanto, os testes de áudio revelaram um problema sério: a saída fica degradada mesmo quando a redução de ruído está desligada. Por isso, a velocidade está demonstrada, mas a qualidade e a fidelidade do processamento ainda não estão validadas.

## Resumo executivo

O objetivo é estudar se é viável implementar redução de ruído em tempo real num microcontrolador Raspberry Pi Pico, no contexto de uma competição de 24 horas. O trabalho está focado em **ruído de banda larga** (por exemplo, vento, hiss ou ruído ambiente), enquanto outros elementos podem estudar ruído tonal/estacionário e ruído transitório/impulsivo.

Foi escolhida uma abordagem clássica de processamento digital de sinal: **subtração espectral**. O áudio é dividido em blocos sobrepostos, convertido para o domínio da frequência através de uma FFT, é estimado o ruído em cada frequência e são aplicados ganhos para atenuar as componentes consideradas ruído. Uma IFFT e uma reconstrução por sobreposição devolvem o áudio ao domínio do tempo.

A primeira implementação utilizava uma FFT Q15 própria. Como o tempo de processamento estava demasiado perto do limite, foi feita uma versão com CMSIS-DSP, que fornece funções otimizadas para FFT e operações vetoriais. Para facilitar os testes sem material analógico, foi depois criada uma interface USB: o computador envia um WAV em blocos PCM, a Pico processa os blocos e devolve o resultado.

Na Pico 2 W física, a 150 MHz, o benchmark mediu **1,053 ms em média e 1,153 ms no pior bloco**, face a um orçamento de 16 ms. Isto demonstra margem de cálculo para o algoritmo atual. Porém, ao testar áudio real, a saída ficou audivelmente pior. O modo de diagnóstico `U` (FFT/IFFT sem redução de ruído) também soa degradado. Portanto, o problema está provavelmente na cadeia de reconstrução/fixed-point, e não apenas na fórmula de redução de ruído.

**Conclusão atual:** prova de conceito computacional promissora; denoiser ainda não validado como sistema de áudio funcional.

---

## 1. Objetivo e enquadramento

A pergunta principal não é «conseguimos construir um produto de áudio completo em 24 horas?». É antes:

- Que técnicas de DSP são adequadas a cada tipo de ruído?
- Que qualidade de redução é possível num microcontrolador pequeno?
- O processamento consegue cumprir o ritmo do áudio?
- Que limitações de memória, cálculo e entrada/saída existem?
- Que partes podem ser preparadas como ponto de partida para os participantes?

A classificação do ruído depende de dimensões diferentes. Um estalo é transitório no tempo, mas pode ocupar muitas frequências. O vento pode ser de banda larga e variar ao longo do tempo. Assim, «banda larga», «tonal» e «transitório» são categorias úteis de trabalho, mas não são completamente exclusivas.

### Âmbito desta parte do trabalho

O foco é ruído de banda larga misturado com áudio que se pretende preservar, provavelmente fala. O objetivo desta fase é avaliar uma abordagem e identificar limitações, não apresentar uma solução perfeita para todos os sons.

---

## 2. Arquitetura escolhida para a prova de conceito

Inicialmente foi considerada uma cadeia de áudio física com microfone ligado ao ADC da Pico e uma saída analógica. Isso exigiria condicionamento do sinal do microfone, amplificação, filtragem e uma solução de saída de áudio. Para evitar que a falta de jacks, DAC, amplificador ou osciloscópio bloqueasse o trabalho, foi escolhida uma abordagem mais simples:

```text
Ficheiro WAV no computador
        |
        |  O programa Python envia PCM por USB
        v
Raspberry Pi Pico 2 W
        |
        |  Recebe blocos de 256 amostras
        v
Algoritmo DSP na Pico
        |
        |  Devolve blocos PCM por USB
        v
Programa Python no computador
        |
        v
Novo ficheiro WAV para ouvir e analisar
```

O USB funciona como transporte de dados. A Pico não abre ficheiros WAV: recebe apenas amostras numéricas. O computador trata de ler o ficheiro, converter o formato, enviar os dados e guardar o resultado.

**Isto não é USB Audio Class.** A Pico não aparece ao sistema operativo como uma placa de som e não recebe áudio em direto de qualquer aplicação. É uma ligação USB CDC/serial com um protocolo simples de comandos e blocos binários. É adequada para testar o DSP com gravações reais sem hardware de áudio externo.

### Formato de áudio utilizado

- Frequência de amostragem: **16 000 amostras/s**.
- Canais: **mono**.
- Amostras: **PCM signed 16-bit**.
- Bloco de transporte: **256 amostras = 512 bytes**.
- Cada bloco representa **16 ms** de áudio.

O script `host/pico_host.py` aceita WAV PCM mono, 16 kHz e 16 bits. Um ficheiro noutro formato pode ser convertido, por exemplo, com:

```bash
ffmpeg -i gravacao.wav -ac 1 -ar 16000 -c:a pcm_s16le entrada.wav
```

### O que esta arquitetura não inclui

- Não usa o ADC da Pico para captar um microfone.
- Não tem saída analógica por DAC ou PWM.
- Não é ainda streaming de áudio em tempo real com cadência garantida.
- Não mede a latência total do sistema, apenas o tempo de cálculo do DSP.

---

## 3. Parâmetros fundamentais do processamento

| Parâmetro | Valor | Significado |
|---|---:|---|
| Frequência de amostragem | 16 kHz | 16 000 amostras por segundo |
| Tamanho da janela (frame) | 512 amostras | 32 ms de áudio por análise |
| Avanço por chamada (hop) | 256 amostras | 16 ms de áudio novo |
| Sobreposição | 50% | Cada janela partilha metade das amostras com a seguinte |
| FFT | 512 pontos | Separa o sinal em componentes de frequência |
| Bins positivos | 257 | De DC até à frequência de Nyquist |
| Espaçamento entre bins | 31,25 Hz | 16 000 / 512 |
| Ganho mínimo | 0,05 | Evita eliminar completamente uma frequência |
| Fator de sobresubtração | 4 | Parâmetro da fórmula de subtração espectral |

Uma frame contém 512 amostras, mas o programa recebe apenas 256 amostras novas de cada vez. As outras 256 vêm do bloco anterior. Isto permite analisar o sinal com janelas sobrepostas e produzir um bloco novo de saída a cada 16 ms.

### Porque usar janelas?

A FFT analisa um segmento finito do sinal. Se o início e o fim desse segmento forem cortados abruptamente, podem surgir componentes artificiais no espectro. A janela **sqrt-Hann** reduz esse efeito. A mesma janela é usada antes e depois da FFT; com 50% de sobreposição, a soma dos produtos das janelas é concebida para reconstruir o sinal.

---

## 4. Como funciona a subtração espectral

A ideia é analisar cada frequência separadamente, estimar quanto dessa frequência se deve ao ruído e atenuá-la.

### 4.1 Passar do tempo para a frequência

O áudio original é uma sequência de amplitudes ao longo do tempo. A FFT converte uma frame dessa sequência num conjunto de componentes de frequência. Em vez de perguntar «qual é a amplitude no instante t?», passamos a perguntar «quanta energia existe perto de cada frequência?».

### 4.2 Estimar o ruído

Para cada bin, o código calcula a potência:

```text
potência = parte_real² + parte_imaginária²
```

A potência é suavizada no tempo: 60% do valor anterior e 40% do valor atual. Depois, a implementação guarda valores mínimos em blocos de 16 frames.

- 16 frames × 16 ms = 256 ms por bloco de estimação.
- Mantêm-se até 5 blocos completos, mais o bloco em construção.
- Cinco blocos completos representam cerca de 1,28 s de histórico.

A estimativa atual usa o mínimo observado em cada bin. É uma aproximação simples e barata, mas tem limitações: pode ficar demasiado baixa, adaptar-se mal a ruído variável ou confundir partes silenciosas da fala com ruído.

### 4.3 Calcular o ganho de cada frequência

O código usa a fórmula:

$$
G(k)=\max\left(1-\sqrt{\frac{4P_N(k)}{P_X(k)}},\;0,05\right)
$$

onde:

- \(P_X(k)\) é a potência observada no bin atual;
- \(P_N(k)\) é a potência estimada do ruído nesse bin;
- \(G(k)\) é o ganho aplicado à frequência.

Interpretação:

- Se a potência estimada do ruído for pequena face à potência observada, o ganho aproxima-se de 1: a frequência mantém-se.
- Se o ruído estimado for elevado, o ganho diminui: a frequência é atenuada.
- O ganho mínimo é 0,05, para evitar remover completamente uma componente.

Depois são aplicadas duas suavizações: uma entre bins vizinhos (frequência) e outra entre frames consecutivas (tempo). Isto tenta evitar que os ganhos mudem de forma demasiado abrupta e reduz os artefactos conhecidos como *musical noise*.

### 4.4 Voltar ao áudio

Os ganhos são aplicados ao espectro. A IFFT converte as componentes de frequência novamente em amostras no tempo. Por fim, a janela de síntese e o overlap-add juntam a parte nova da frame à parte guardada da frame anterior.

O resultado é um bloco com 256 amostras processadas.

---

## 5. Como o código está organizado

O projeto de prova de conceito tem duas partes principais: firmware C na Pico e um programa Python no computador.

### `main.c` — comunicação e execução

`main()` inicializa USB, cria o estado do denoiser e fica à espera de comandos. Não contém a lógica matemática principal.

As funções importantes são:

- `read_line()` — lê um comando de texto, como `B` ou `S 100`.
- `usb_read_exact()` — recebe um bloco completo de bytes, mesmo que o USB o entregue em partes.
- `usb_write_exact()` — envia um bloco de bytes sem o converter em texto.
- `run_benchmark()` — gera um sinal de teste dentro da Pico, chama o DSP e mede o tempo.
- `run_usb_stream()` — recebe blocos PCM, escolhe o modo de processamento e devolve os blocos ao computador.

### `denoiser.c` — processamento digital de sinal

É aqui que está o algoritmo. As funções principais são:

- `denoiser_init()` — limpa o estado, inicializa a FFT CMSIS-DSP, prepara a janela sqrt-Hann e os históricos.
- `process_hop()` — função central: atualiza a janela, aplica a janela de análise, executa FFT, opcionalmente calcula e aplica ganhos, executa IFFT e reconstrói a saída.
- `calculate_power()` — calcula a potência por frequência e suaviza-a.
- `calculate_gains()` — compara a potência atual com a estimativa de ruído, calcula os ganhos e suaviza-os.
- `calculate_bin_gain()` — calcula o ganho de um bin individual.
- `apply_symmetric_gains()` — aplica o mesmo ganho às frequências simétricas, para manter a simetria do espectro de um sinal real.
- `finish_noise_block()` — termina um bloco de estimação do ruído e guarda-o no histórico.
- `denoiser_process_hop()` — executa o caminho normal, com redução de ruído.
- `denoiser_process_hop_unity()` — executa o caminho FFT/IFFT sem calcular nem aplicar ganhos de redução.

### `denoiser.h` — parâmetros e estado

Define as dimensões dos blocos e a estrutura `Denoiser`, que contém os buffers da FFT, a janela, o histórico de entrada, o buffer de overlap-add, as potências, os ganhos e o histórico de ruído.

O estado deve ser global ou estático. Não deve ser colocado na stack de uma função, porque contém vários buffers de dimensão significativa.

### `host/pico_host.py` — interface no computador

O script Python:

1. Abre o WAV e verifica o formato.
2. Divide as amostras em blocos de 256.
3. Envia o comando adequado e os bytes PCM pela porta série USB.
4. Lê o bloco de saída correspondente.
5. Junta os blocos e grava um novo WAV.

O script compensa o primeiro bloco de arranque nos modos `unity` e `denoise`, porque a reconstrução por sobreposição introduz um atraso inicial de um hop (16 ms). O modo `echo` não tem esse atraso.

---

## 6. Comandos USB disponíveis

O protocolo usa a mesma porta USB CDC para comandos em texto e dados de áudio binários.

| Comando | O que faz | Para que serve |
|---|---|---|
| `P` | A Pico responde `PONG` e volta a anunciar `READY` | Sincronizar o programa Python com a Pico |
| `B` | Gera um sinal sintético e mede o DSP | Benchmark de desempenho |
| `E N` | Recebe N blocos e devolve exatamente os mesmos bytes | Isolar o transporte USB e o script |
| `U N` | Executa FFT/IFFT e overlap-add sem redução | Testar a fidelidade da reconstrução |
| `S N` | Executa o algoritmo completo de subtração espectral | Testar a redução de ruído |

Cada bloco de áudio tem 512 bytes: 256 amostras × 2 bytes. O programa do computador envia um bloco e espera pela resposta antes de enviar o seguinte, para não encher os buffers.

### Porque foi adicionado o comando `P`?

Na primeira versão, o script esperava receber uma linha `READY` depois de abrir a porta. Porém, a Pico podia já ter enviado essa mensagem antes de o script começar a ouvir. Os dois lados ficavam então à espera. O comando `P` resolve a sincronização: o computador envia `P`, a Pico responde `PONG`, e só depois o script prossegue.

---

## 7. Evolução do trabalho

### Etapa 1 — FFT Q15 própria

A primeira implementação usava uma FFT radix-2 iterativa escrita especificamente para o projeto. Como os cálculos eram feitos em Q15, foi necessário controlar o crescimento dos valores em cada etapa para evitar overflow.

Resultados exploratórios reportados em Wokwi:

| Versão | Média por hop | Pior hop | Orçamento |
|---|---:|---:|---:|
| FFT própria, primeira versão | 25,709 ms | 26,272 ms | 16 ms |
| FFT própria, otimizada | 15,328 ms | 16,040 ms | 16 ms |

A versão otimizada ficou demasiado perto do limite e ultrapassou-o no pior caso.

### Etapa 2 — CMSIS-DSP Q15

A FFT própria foi substituída por funções da biblioteca CMSIS-DSP, mantendo a abordagem de subtração espectral e os cálculos de ganho em ponto fixo.

Num ensaio em Wokwi, foram reportados cerca de 4,286 ms em média e 4,410 ms no pior caso. Este resultado foi promissor, mas o simulador não deve ser usado como referência final de desempenho físico.

### Etapa 3 — Entrada e saída por USB

Foi criada uma prova de conceito em que o computador envia um WAV e recebe outro. Esta decisão evitou depender de microfone, pré-amplificador, cabos jack, DAC ou osciloscópio.

Foram acrescentados os modos `B`, `E`, `U` e `S`, além do comando de sincronização `P`.

### Etapa 4 — Compilação para Pico 2 W

A primeira compilação com Arm GCC 15.2 falhou durante a compilação de uma função CMSIS-DSP não relacionada diretamente com o denoiser (`arm_abs_q31.c`). O erro era um *internal compiler error* do próprio compilador, durante uma etapa de otimização, e não um erro reportado no código do projeto.

Depois de mudar para Arm GNU Toolchain 15.3 e limpar a pasta `build`, o utilizador conseguiu compilar o projeto.

---

## 8. Resultados medidos na Pico 2 W

O utilizador executou o benchmark na Pico 2 W física. O firmware reportou o relógio a **150 MHz**.

| Medida | Resultado |
|---|---:|
| Blocos processados | 188 |
| Frequência de amostragem | 16 kHz |
| Média do DSP por bloco | **1,053 µs** |
| Melhor bloco | 993 µs |
| Pior bloco | 1,153 µs |
| Blocos acima de 16 ms | **0** |
| Orçamento por bloco | 16,000 µs |

A 16 kHz, 256 amostras correspondem a 16 ms. O pior tempo observado, 1,153 ms, representa aproximadamente 7,2% desse orçamento. Em termos de cálculo, há margem significativa.

**O que o benchmark prova:** a Pico 2 W consegue executar a função DSP atual com rapidez suficiente para o ritmo de 16 ms por bloco, neste ensaio sintético.

**O que não prova:** que o áudio é reconstruído corretamente, que o ruído é reduzido, que o som fica melhor, que a transferência USB é suficientemente rápida para áudio em direto ou que o RP2040 original terá o mesmo desempenho.

O benchmark gera áudio dentro da Pico: primeiro ruído, depois um tom de 440 Hz misturado com ruído. Mede apenas a chamada `denoiser_process_hop()`. Não mede USB, ADC, DMA, saída de áudio nem qualidade percetiva.

> **Importante:** a Pico 2 W usa RP2350/Cortex-M33. A placa original Pico usa RP2040/Cortex-M0+. Não se deve apresentar o resultado da Pico 2 W como resultado de desempenho do RP2040.

---

## 9. Testes com áudio real e o problema encontrado

Depois do benchmark, foi testada uma gravação real de aproximadamente **30,44 s**, em mono, 16 kHz e PCM de 16 bits.

### 9.1 Comparação objetiva do ficheiro original com `denoised.wav`

As medições feitas aos ficheiros fornecidos foram:

| Medida | Original | `denoised.wav` |
|---|---:|---:|
| Duração | 30,441 s | 30,441 s |
| Média (DC) | +0,05 | **−430,08** |
| RMS global | 981,0 | 1 040,3 |
| Desvio-padrão global | 981,0 | 947,3 |
| Pico absoluto | 7 328 | 7 380 |

A saída apresenta um desvio DC negativo importante, que não existia no original. O RMS global aumentou, em parte por causa desse desvio.

A parte final da gravação (aproximadamente 28,0 s até ao fim), que é relativamente silenciosa, é ainda mais reveladora:

| Parte final | Original | `denoised.wav` |
|---|---:|---:|
| Média | aproximadamente 0 | −380,6 |
| Desvio-padrão | 143,7 | **507,1** |

Ou seja, a energia variável nessa parte silenciosa ficou cerca de 3,5 vezes maior. Isto está de acordo com a observação auditiva do utilizador: a voz continua relativamente clara, mas fica rodeada por muito mais ruído.

### 9.2 Resultado do modo `E` — echo USB

O modo `E` não executa FFT nem redução de ruído: copia os bytes de entrada para a saída. O utilizador ouviu o resultado e considerou-o essencialmente igual ao original, talvez ligeiramente mais baixo; reconheceu que a diferença de volume poderia ser apenas uma impressão auditiva.

O script também inclui uma comparação byte a byte e pode imprimir `USB byte-for-byte check: PASS`. Essa confirmação textual não foi registada na conversa, pelo que a integridade digital do echo ainda deve ser confirmada explicitamente.

### 9.3 Resultado do modo `U` — unity

O modo `U` executa a cadeia de janela, FFT, IFFT e overlap-add, mas **não aplica a estimativa de ruído nem os ganhos de redução**.

O utilizador reportou que `unity.wav` soa muito pior: o ruído parece ainda mais amplificado, apesar de a voz continuar relativamente clara.

Este é o resultado mais importante do diagnóstico. Se o modo unity já degrada o áudio, então não faz sentido começar por afinar a força da subtração espectral. Primeiro é necessário corrigir e validar a reconstrução do sinal.

---

## 10. O que está provavelmente errado — e o que ainda não sabemos

A implementação usa `arm_cfft_q15()` para a FFT e para a IFFT. Depois da IFFT, o código multiplica cada amostra por:

```c
1u << DENOISER_CFFT_SCALE_BITS
```

com `DENOISER_CFFT_SCALE_BITS` definido como 9, ou seja, um fator de 512. O resultado é limitado a 16 bits antes da janela de síntese e do overlap-add.

Esta compensação foi introduzida com base numa interpretação da escala interna da FFT Q15. A documentação da CMSIS-DSP mostra que as representações e regras de escala da CFFT e da CIFFT são específicas; não se deve assumir que a mesma regra serve para todas as transformações e comprimentos. A escala exata do caminho de 512 pontos tem de ser confirmada para a versão usada (v1.18.0), antes de manter ou alterar o fator 512.

Há uma segunda questão concreta no código: **não existe normalização dinâmica da entrada antes da FFT**. A gravação de teste tem pico de apenas 7 328 numa escala de 32 767 (cerca de 22% da escala disponível). A FFT Q15 faz operações internas com precisão limitada; se os valores intermédios ficarem demasiado pequenos, os arredondamentos e a quantização podem tornar-se significativos. A biblioteca Arm documenta exemplos em que a escala do sinal é tratada separadamente da escala inerente à FFT/IFFT.

Outros aspetos a investigar:

1. **Precisão Q15:** o nível relativamente baixo do áudio pode deixar pouca margem útil depois das escalas internas e dos arredondamentos.
2. **Escala e saturação:** depois da IFFT, o código multiplica por 512 e limita o resultado a `int16_t`. Se a escala assumida não corresponder à saída real da CIFFT, pode amplificar erros ou provocar saturação. O código atual não conta quantas amostras saturam.
3. **Reconstrução por overlap-add:** é necessário confirmar que janela de análise, janela de síntese, escala da IFFT e alinhamento temporal produzem uma saída unity praticamente igual à entrada.
4. **Estimativa do ruído:** o mínimo por bin em blocos de tempo é uma aproximação simples e pode produzir ganhos inadequados, mas não explica por si só a degradação observada em `U`, onde os ganhos nem sequer são aplicados.

**Causa confirmada até agora:** a cadeia unity/reconstrução degrada o áudio.  
**Causas prováveis a testar:** precisão/normalização Q15, compensação de escala da CIFFT e saturação. Ainda não foi isolada uma causa única.

---

## 11. Estado atual do projeto

| Componente | Estado atual |
|---|---|
| Compilação para Pico 2 W | **Conseguida pelo utilizador** com Arm GCC 15.3 |
| Comunicação USB CDC | Implementada |
| Receção de WAV por blocos PCM | Implementada |
| Devolução de WAV processado | Implementada |
| Benchmark sintético na Pico 2 W | **Passou**, com margem de cálculo |
| Modo echo (`E`) | Implementado; confirmação byte a byte ainda deve ser registada |
| Modo unity (`U`) | Implementado, mas **falha em qualidade auditiva** |
| Subtração espectral (`S`) | Implementada, mas a qualidade não é válida enquanto unity falhar |
| Entrada real por ADC/microfone | Não implementada |
| Saída analógica | Não implementada |
| USB Audio Class / áudio em direto | Não implementado |
| Reconstrução fiel do áudio | **Não validada; problema conhecido** |
| Eficácia de redução de ruído | **Não demonstrada** |

---

## 12. Próximos passos recomendados

A ordem de trabalho deve evitar afinar um algoritmo que ainda não reconstrói corretamente o sinal.

### Passo 1 — Confirmar a integridade do caminho USB

Executar `E` e confirmar que o script imprime `USB byte-for-byte check: PASS`. Se falhar, investigar primeiro a receção, o envio e a gravação WAV.

### Passo 2 — Corrigir a reconstrução unity

Desligar toda a redução de ruído e testar apenas:

1. geração de sinais de teste simples (impulso, senoide, mistura de tons e gravação real);
2. janela sqrt-Hann;
3. CFFT Q15 de 512 pontos;
4. CIFFT Q15 de 512 pontos;
5. conversão de escala e saturação;
6. janela de síntese e overlap-add.

Registar para cada teste: ganho entrada/saída, erro RMS, desvio DC, pico máximo e número de amostras saturadas. A saída unity deve ser praticamente igual à entrada, depois de corrigir o atraso de 16 ms.

### Passo 3 — Validar a escala Q15

Consultar e testar a escala concreta da CFFT/CIFFT de 512 pontos na CMSIS-DSP v1.18.0. Não assumir automaticamente que multiplicar por 512 depois da IFFT é correto. Considerar normalização/escala adaptativa da entrada para aproveitar melhor o intervalo Q15 e compensar essa escala de forma consistente. Avaliar também o uso de Q31 em cálculos de potência.

### Passo 4 — Só depois voltar à redução de ruído

Quando o modo unity passar, avaliar a subtração espectral com gravações controladas. Idealmente, usar uma gravação com uma pequena secção inicial de ruído sem fala para obter uma estimativa inicial do ruído. Comparar sempre áudio original, áudio com ruído conhecido e áudio processado.

### Passo 5 — Medir qualidade, não apenas velocidade

Além do tempo por bloco, medir SNR quando existe uma referência limpa, erro RMS, níveis de ruído nos intervalos sem fala e qualidade auditiva. Um benchmark rápido não é, por si só, um denoiser eficaz.

### Passo 6 — Só depois integrar ADC e saída física

Para uma versão autónoma, será necessário um microfone com pré-amplificação e condicionamento adequados à entrada ADC, aquisição temporizada (idealmente com DMA) e uma saída de áudio por DAC externo/codec ou PWM com filtragem e amplificação. Não é necessário para validar primeiro o algoritmo com WAV por USB.

---

## 13. O que é possível afirmar na reunião

Uma apresentação honesta e defensável pode ser feita assim:

> Estamos a estudar subtração espectral para redução de ruído de banda larga em áudio. Implementámos um processamento por frames de 512 amostras, com avanço de 256 amostras, a 16 kHz, usando aritmética Q15 e CMSIS-DSP. Para testar sem hardware analógico, a Pico recebe WAVs por USB a partir de um computador e devolve o áudio processado. Na Pico 2 W, a 150 MHz, o algoritmo correu em cerca de 1,05 ms por bloco em média, com 1,15 ms no pior caso, muito abaixo dos 16 ms disponíveis. Portanto, o custo computacional parece viável. No entanto, os testes de áudio revelaram que até o modo sem redução de ruído, que apenas faz FFT/IFFT e reconstrução, degrada o som. Estamos agora a tratar a fidelidade da reconstrução e a escala Q15 antes de avaliar a eficácia da redução de ruído. Ainda não consideramos o denoiser funcional em termos de qualidade sonora.

### Se perguntarem «então já funciona?»

A resposta correta é: **o firmware compila, a comunicação USB funciona e o DSP corre dentro do orçamento de cálculo; mas a reconstrução de áudio ainda tem um problema e a redução de ruído não está validada.**

### Se perguntarem «porque é que não usam já o ADC?»

Porque a entrada USB permite separar o algoritmo dos problemas de hardware analógico. Assim é possível testar com gravações reais sem microfone, pré-amplificador, DAC, jack ou osciloscópio. O ADC e a saída analógica são uma etapa de integração posterior.

### Se perguntarem «o que significa o benchmark passar?»

Significa que a função DSP demora menos do que os 16 ms disponíveis por bloco. Não diz se o resultado sonoro é melhor, nem mede a transferência USB ou o sistema completo.

---

## 14. Glossário rápido

| Termo | Explicação simples |
|---|---|
| ADC | Conversor que transforma uma tensão analógica numa sequência de números |
| DAC | Conversor que transforma números digitais num sinal analógico |
| Amostra | Um valor de amplitude do áudio num instante |
| Frame | Janela de amostras analisada de cada vez |
| Hop | Número de amostras novas entre análises consecutivas |
| FFT | Algoritmo que separa um sinal nas suas frequências |
| IFFT | Operação inversa: reconstrói o sinal a partir das frequências |
| Bin | Uma posição do espectro correspondente a uma frequência |
| Espectro | Representação do sinal em função da frequência |
| Subtração espectral | Atenuar frequências de acordo com a estimativa do ruído |
| Q15 | Formato de ponto fixo de 16 bits, usado para representar números fracionários |
| Saturação | Limitar um valor ao máximo/mínimo que cabe no formato numérico |
| Overlap-add | Somar partes sobrepostas de frames para reconstruir o áudio contínuo |
| RMS | Medida do nível médio do sinal |
| DC offset | Deslocamento da média do sinal para longe de zero |
| Musical noise | Artefactos tonais curtos que podem surgir em métodos de subtração espectral |
| CMSIS-DSP | Biblioteca da Arm com funções otimizadas de processamento digital de sinal |

---

## Referências técnicas

1. Arm, **CMSIS-DSP — Complex FFT Q15**: documentação da função `arm_cfft_q15()`, parâmetros, formatos Q15 e informação de escala. <https://arm-software.github.io/CMSIS-DSP/main/group__ComplexFFTQ15.html>
2. Arm, **Write the CMSIS-DSP Q15 implementation**: exemplo e discussão de escalamento e precisão em fluxos Q15. <https://learn.arm.com/learning-paths/embedded-and-microcontrollers/cmsisdsp-dev-with-python/how-to-6/>
3. Arm, **CMSIS-DSP**: biblioteca de funções DSP para microcontroladores Cortex-M. <https://github.com/ARM-software/CMSIS-DSP>
4. Raspberry Pi, **Pico 2 documentation**: documentação oficial da família Pico 2/RP2350. <https://www.raspberrypi.com/documentation/microcontrollers/raspberry-pi-pico.html>

---

## Anexo A — Comandos de utilização

### Compilar para Pico 2 W

```bash
cmake -S . -B build -G Ninja \\
  -DPICO_BOARD=pico2_w \\
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

O UF2 gerado encontra-se em `build/pico_usb_denoiser.uf2`. Para a Pico original, selecionar `-DPICO_BOARD=pico` e voltar a configurar a pasta `build`.

### Benchmark

```bash
python3 host/pico_host.py --port /dev/ttyACM0 --benchmark
```

### Eco USB

```bash
python3 host/pico_host.py --port /dev/ttyACM0 \\
  --input entrada.wav --output eco.wav --echo
```

### Reconstrução unity

```bash
python3 host/pico_host.py --port /dev/ttyACM0 \\
  --input entrada.wav --output unity.wav --unity
```

### Redução de ruído

```bash
python3 host/pico_host.py --port /dev/ttyACM0 \\
  --input entrada.wav --output filtrado.wav
```

