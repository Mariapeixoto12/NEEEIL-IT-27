# Redução de ruído de banda larga — estado do trabalho

## Ideia e objetivo

Estamos a avaliar a viabilidade de fazer redução de ruído de banda larga em áudio com uma Raspberry Pi Pico, no contexto de uma competição de 24 horas. O objetivo desta fase é perceber se uma técnica de DSP pode correr dentro do orçamento temporal e que limitações de qualidade e hardware existem — não apresentar já um produto final.

Foi escolhida **subtração espectral**: dividir o áudio em blocos, passar cada bloco para o domínio da frequência, estimar o ruído por frequência, atenuar as componentes consideradas ruído e reconstruir o áudio.

## O que foi implementado

- Áudio mono, PCM 16-bit, a 16 kHz.
- Frames de 512 amostras (32 ms) com avanço de 256 amostras (16 ms) e 50% de sobreposição.
- FFT Q15 através da biblioteca CMSIS-DSP.
- Janela sqrt-Hann, estimativa de ruído por mínimos ao longo de blocos, ganhos com fator de sobresubtração 4 e ganho mínimo 0,05.
- Interface USB CDC: o computador envia um WAV como blocos PCM; a Pico devolve os blocos processados; o computador grava o WAV de saída.
- Modos de diagnóstico: `E` (eco USB), `U` (FFT/IFFT sem redução) e `S` (redução espectral); `B` mede o desempenho e `P` sincroniza a ligação.

A interface USB foi escolhida para testar gravações reais sem microfone, pré-amplificador, jack, DAC, amplificador ou osciloscópio. **Não é USB Audio Class nem entrada ADC**: a Pico ainda não é uma placa de som e não capta diretamente um microfone.

## Resultado de desempenho na Pico 2 W física

A 150 MHz, o benchmark sintético processou 188 blocos:

| Medida | Resultado |
|---|---:|
| Tempo médio DSP | **1,053 ms/bloco** |
| Melhor bloco | 0,993 ms |
| Pior bloco | 1,153 ms |
| Orçamento disponível | 16 ms/bloco |
| Blocos acima do orçamento | **0** |

Isto é um bom resultado: o cálculo usa cerca de 7,2% do orçamento no pior bloco observado. Mas o benchmark mede apenas a função DSP com um sinal sintético; não mede USB, ADC, saída de áudio nem qualidade sonora. O resultado é da **Pico 2 W / RP2350**, não da Pico original / RP2040.

## O problema encontrado nos testes de áudio

Foi usado um WAV real de 30,44 s. O modo normal (`S`) produziu um resultado audivelmente pior: a voz continuava relativamente clara, mas surgiu muito mais ruído. A análise dos ficheiros mostrou também um desvio DC de cerca de −430 contagens na saída. Na parte final, relativamente silenciosa, o desvio-padrão passou de cerca de 144 (original) para 507 (processado).

O modo `E` apenas copia os bytes recebidos. Ao ouvir o resultado, pareceu essencialmente igual ao original; ainda falta registar explicitamente o resultado da comparação byte a byte do script.

O modo `U` é o diagnóstico decisivo: executa janela, FFT, IFFT e overlap-add, **mas não aplica redução de ruído**. Mesmo assim, o utilizador ouviu bastante degradação. Portanto, não devemos afinar ainda a fórmula de subtração: primeiro temos de corrigir a reconstrução.

## Principal questão técnica em aberto

A implementação usa aritmética Q15. Não faz normalização dinâmica antes da FFT e, depois da IFFT, aplica uma compensação fixa de escala de 512 antes de limitar os valores a 16 bits. As convenções de escala da CFFT e da CIFFT CMSIS-DSP são específicas; a escala concreta do caminho de 512 pontos tem de ser confirmada para a versão v1.18.0. A entrada de teste também usa apenas cerca de 22% da escala Q15 no pico, o que pode tornar os efeitos de quantização relevantes.

Isto é uma hipótese prioritária, não uma causa já provada. Também é preciso contar amostras saturadas e validar janela e overlap-add.

## Próxima sequência de trabalho

1. Confirmar `E`: o script deve indicar `USB byte-for-byte check: PASS`.
2. Fazer `U` reconstruir o sinal praticamente sem alteração: medir erro RMS, ganho, DC offset, picos e saturações com impulso e senoides.
3. Confirmar a escala da FFT/IFFT Q15 de 512 pontos e implementar normalização/compensação de escala corretamente.
4. Só depois voltar a testar `S`, melhorar a estimativa de ruído e medir a melhoria com sinais de referência.
5. Depois integrar ADC/DMA e uma saída física, caso seja necessário um sistema autónomo.

## Mensagem que podemos defender

**A viabilidade computacional parece promissora:** na Pico 2 W, o DSP corre muito abaixo do orçamento de 16 ms. Porém, os testes revelaram um problema de fidelidade na reconstrução FFT/IFFT, mesmo sem redução de ruído. Assim, o algoritmo ainda não está validado como denoiser. O trabalho atual é corrigir primeiro a reconstrução e só depois avaliar a redução de ruído.

## Estado resumido

| Parte | Estado |
|---|---|
| Firmware compila na Pico 2 W | Sim |
| Comunicação e transporte USB | Implementados; falta registar PASS byte a byte |
| Benchmark de cálculo | Passou |
| Reconstrução unity | **Falha auditiva; por corrigir** |
| Redução de ruído | Implementada, mas **não validada** |
| ADC/microfone e saída analógica | Não implementados |
