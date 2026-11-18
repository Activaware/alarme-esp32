# Conferência pino a pino: código × esquemáticos

Fontes comparadas:

| Sigla | Arquivo |
|---|---|
| **FW** | `firmware/controlador/controlador.ino` e `firmware/transmissor/transmissor.ino` |
| **DES** | `hardware/esquematico_controlador.jpg` (desenho com notas) e `hardware/esquematico_chaveiro.svg` |
| **EDA** | `hardware/esquematico_controlador_easyeda.png` (EasyEDA, folha 1/1) |

Legenda: ✅ confere · ⚠️ atenção · ❌ divergência.

> O EasyEDA foi lido em imagem de baixa resolução. Onde há ❌/⚠️ nessa coluna,
> **abra o projeto no EasyEDA e confirme** os nomes de net antes de mandar a placa para fabricação.

## Controlador veicular

| # | Função | FW (`#define`) | DES | EDA | Resultado |
|---|---|---|---|---|---|
| 1 | Relé travar | `PIN_TRAVA` = GPIO23 | IN1 TRAVA ← GPIO23 | `SaidaTrava` em IO23 → P1.1 | ✅ |
| 2 | Relé destravar | `PIN_DESTRAVA` = GPIO22 | IN2 DESTRAVA ← GPIO22 | `SaidaDestrava` em IO22 → P1.2 | ✅ |
| 3 | Relé sirene | `PIN_SIRENE` = GPIO21 | IN3 SIRENE ← GPIO21 | `SaidaSirene` em IO21 → P1.3 | ✅ |
| 4 | Relé pisca | `PIN_PISCA` = GPIO19 | IN4 PISCA ← GPIO19 | `SaidaRelePisca` em IO19 → P2.1 | ✅ |
| 5 | Relé corte da bomba | `PIN_CORTE` = GPIO18 | IN5 CORTE ← GPIO18 (contato NF) | `SaidaCorteBomba` em IO18 → P2.2 | ✅ |
| 6 | Sensor de porta | `PIN_PORTA` = GPIO32, `INPUT_PULLUP`, LOW = aberta | PC817 coletor → GPIO32 ("pull-up interno"), emissor → GND | net `PullUp` → IO32; R2 2,2 kΩ do 12 V ao LED do PC817 | ✅ |
| 7 | Sensor de choque | `PIN_CHOQUE` = GPIO27 | SW-420 `DO` → GPIO27, VCC 3V3 | **não existe SW-420 nem net em IO27** | ❌ **EDA incompleto** |
| 8 | Ignição (ADC) | `PIN_IGNICAO` = GPIO34 | 47 kΩ + 10 kΩ + 100 nF → GPIO34 | R3 47k, R4 10k, C1 100 nF → `12VIGNOUT` → IO34 | ✅ |
| 9 | GPS TX → ESP RX | `GPS_RX` = GPIO16 | GPS `TX` → GPIO16 (RX2) | nets `RX3`/`TX2` ligadas aos pinos **TX / RX (GPIO1 / GPIO3)** do DevKit, não a IO16/IO17 | ❌ **EDA diferente do código** |
| 10 | GPS RX ← ESP TX | `GPS_TX` = GPIO17 | GPS `RX` ← GPIO17 (TX2) | idem | ❌ idem |
| 11 | LED de status | `PIN_LED` = GPIO2, 330 Ω | 330 Ω + LED → GND | `ledStatus` em IO2, R1 330 Ω | ✅ |
| 12 | Alimentação do ESP32 | VIN = +5 V | Buck 12→5 V → VIN | LM2596 → `In5V` → pino 5V | ✅ |
| 13 | Alimentação GPS e SW-420 | 3V3 do ESP32 | 3V3 | 3V3 (GPS) | ✅ |
| 14 | Proteção da entrada | — | fusível 2 A + TVS SMBJ24A | fusível 5×20 mm + TVS (MSMBJ24A) | ⚠️ ver item A |

### Divergências e como resolver

**❌ 7 — EasyEDA sem o sensor de choque.** O código lê o GPIO27 e o desenho `.jpg` tem o SW-420, mas a
folha do EasyEDA não tem o módulo nem um conector para ele. Adicione um conector de 3 pinos (3V3, GND, DO)
ligado a IO27, ou o `detectouChoque()` nunca verá nada.

**❌ 9/10 — GPS ligado a GPIO1/GPIO3 no EasyEDA.** No EasyEDA, as nets do NEO-6M (`RX3`, `TX2`) estão
nos pinos `TX` e `RX` do DevKit (UART0, a mesma da USB), e IO16/IO17 ficam sem net. O firmware usa
UART2 em GPIO16/17. Há duas saídas:
- **(recomendada)** corrigir o EasyEDA: `TX` do GPS → **IO16**, `RX` do GPS → **IO17**;
- ou mudar o firmware para `Serial` (UART0), o que **quebra o upload e o monitor serial** com o GPS ligado. Não recomendo.

Lembrete: o cruzamento é TX↔RX. O **TX do GPS** vai no pino que o firmware chama de `GPS_RX` (16).

**⚠️ A — TVS.** Confira no EasyEDA que o SMBJ24A está em **paralelo** (catodo no +12 V após o fusível,
anodo no GND), e não em série. No `.jpg` ele aparece corretamente na entrada do buck.

### Verificações elétricas adicionais (nos dois esquemáticos)

| Ponto | Observação |
|---|---|
| Divisor da ignição | 47 kΩ/10 kΩ → 0,175 × Vin. 12–14,4 V dá 2,1–2,5 V ✔. **Picos de alternador (load dump) chegam a dezenas de volts** → 4,2 V+ no GPIO34. Adicione um **zener 3,3 V (ou TVS) em paralelo com o 10 kΩ**. |
| PC817 da porta | LED com 2,2 kΩ: ≈ 4,9 mA a 12 V ✔. O PC817 suporta só **6 V reversos**; se o fio de porta ficar em +12 V com o módulo desligado/fusível aberto, o LED vê 12 V reverso. Coloque um **1N4148 em antiparalelo** com o LED do opto. |
| Polaridade do fio de porta | Os dois desenhos assumem **porta = negativo ao abrir** (típico de lâmpada de cortesia). Meça no seu carro; se for positivo, a interface muda (e o código passa a ver nível invertido). |
| Módulo de 5 relés | O desenho diz "ativo em ALTO". A maioria dos módulos com optoacoplador é **ativa em BAIXO** → use `#define RELE_ON LOW`. Com VCC 5 V e sinal de 3,3 V, alguns módulos não desligam por completo com GPIO em ALTO; se algum relé ficar "colado", teste. Se o relé do **corte da bomba** ficar ligado sem comando, o carro não pega. |
| SW-420 | O nível do `DO` em vibração varia entre módulos. Ajuste `CHOQUE_ATIVO` no `.ino` (HIGH/LOW) depois de testar com o Serial. |
| ADC | GPIO34 é ADC1 (continua funcionando com Wi-Fi/BLE ligados) ✔. |
| Boot | GPIO2 (LED) é pino de *strapping*: o LED 330 Ω → GND é seguro. GPIO16/17 são usados por PSRAM em módulos **WROVER**; use **WROOM**. |
| Buck | SMBJ24A prende em ≈ 38,9 V; LM2596 padrão suporta 40 V (versão HV, 60 V, é mais folgada). Prefira buck automotivo de boa procedência. |

## Chaveiro

| # | Função | FW | DES (SVG) | Resultado |
|---|---|---|---|---|
| 1 | Botão ARMAR | `BTN_ARMAR` = GPIO25 | GPIO25 ↔ 3V3 | ✅ |
| 2 | Botão DESARMAR | `BTN_DESARMAR` = GPIO26 | GPIO26 ↔ 3V3 | ✅ |
| 3 | Botão PÂNICO | `BTN_PANICO` = GPIO27 | GPIO27 ↔ 3V3 | ✅ |
| 4 | LED | `PIN_LED` = GPIO2, 330 Ω | GPIO2 → 330 Ω → LED | ✅ |
| 5 | Wake-up | ext1 `ANY_HIGH` + pull-down RTC | botões ativos em ALTO | ✅ (GPIO25/26/27 são RTC) |
| 6 | Alimentação | — | LiPo → MCP1700-3302 → 3V3 | ⚠️ ver abaixo |

⚠️ **Chaveiro — pontos de hardware**
- **MCP1700 entrega no máximo 250 mA**, e o ESP32 em transmissão tem picos acima disso. Use capacitor
  de **100–220 µF** no 3V3 (além dos 10 µF) ou um LDO de baixo consumo com mais corrente (500 mA).
- O desenho não mostra **carregador/proteção da LiPo** (ex.: TP4056 com proteção) nem o circuito mínimo do
  módulo WROOM nu (pull-up de EN, acesso a GPIO0 e TX/RX para gravar). Em DevKit, o regulador AMS1117 e o
  chip USB consomem mA mesmo em deep sleep.
