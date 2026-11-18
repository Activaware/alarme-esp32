# Revisão dos firmwares e do app

Resumo do que foi encontrado e alterado em relação aos arquivos originais.
**Nada foi compilado nem testado em hardware** (sem Arduino/Flutter no ambiente de revisão): compile,
leia os avisos e teste na bancada (veja o checklist no README).

## Mudanças de estrutura
| Antes | Depois |
|---|---|
| `protocolo.h` e `protocolo_1.h` (idênticos) | um só em `firmware/common/`, copiado para os dois sketches por `tools/configurar.py` |
| Chave HMAC, PIN e MACs dentro de `.ino`/`.h` | `segredos.h` gerado, **fora do git**, com chave e PIN aleatórios |
| Cópia manual de arquivos | `python tools/configurar.py` (e `--verificar` para CI) |

## Controlador (`controlador.ino`)

| Sev. | Problema | Correção |
|---|---|---|
| Alta | `cmdBle` (String) era escrita pela task BLE e lida no `loop()` sem proteção; dois comandos seguidos perdiam o primeiro ou corrompiam a String. | Fila FreeRTOS (`xQueueCreate(4, 24)`). |
| Alta | Notificações BLE: só a característica tinha criptografia; o **CCCD (0x2902)** aceitava escrita sem pareamento, então um celular não pareado podia assinar estado e GPS. | Permissão `ENC_MITM` no CCCD (remova se seu core criar o CCCD sozinho). |
| Média | Sensores sem filtro: um pico na linha de porta/ignição disparava a sirene. | `persistente()`: porta ≥ 50 ms, ignição ≥ 300 ms (sobe devagar, desce na hora). |
| Média | Ignição: `analogRead() > 1500` sem calibração nem histerese. | `analogReadMilliVolts()` + histerese (liga > 1300 mV, desliga < 900 mV). |
| Média | Sensor de choque contava repique do comparador como "2ª pancada". | Intervalo mínimo de 40 ms entre pulsos; nível ativo configurável (`CHOQUE_ATIVO`). |
| Média | Com a porta aberta ou sensor travado, a sirene reiniciava a cada 30 s **para sempre**. | `MAX_DISPAROS = 3` ciclos; depois fica mudo até desarmar. |
| Baixa | Condição de envio de GPS tinha trecho redundante. | Simplificada. |
| Baixa | Comentário obsoleto em `executarComandoRadio`. | Corrigido. |
| Info | Log no Serial das transições de estado e dos comandos BLE. | Adicionado. |

## Chaveiro (`transmissor.ino`)

| Sev. | Problema | Correção |
|---|---|---|
| Alta (provável) | Os pull-downs RTC eram configurados **antes** de `esp_sleep_enable_ext1_wakeup()`. No IDF 5.x (core 3.x) o ext1 reinicializa os pinos RTC, o que pode apagar os pull-downs e deixar as entradas flutuando (o chip acorda sozinho e gasta bateria). | Ordem invertida + `ESP_PD_DOMAIN_RTC_PERIPH` ligado. |
| Alta | Botão segurado > 3 s: o chip dormia com o pino em ALTO, acordava na hora e **repetia o comando** (pânico alternava ligado/desligado). | `aguardarSoltar()` espera todos os botões (até 15 s). |
| Média | "1 piscada longa = OK" significava só "o carro recebeu". `ARMAR` com porta aberta também dava OK. | O carro devolve o estado; 2 piscadas médias = recebido mas recusado. |
| Baixa | Qualquer wake-up diferente de ext1 mostrava o MAC e dormia 5 s. | Só na energização inicial (`WAKEUP_UNDEFINED`). |

## App (`main.dart` + `comandos_voz.dart`)

| Sev. | Problema | Correção |
|---|---|---|
| **Alta** | `_mapear` usava `contains('arm')`: "status do alarme", "situação do alarme", "o alarme está armado?" e "está ativo?" viravam **ARMAR**. | Novo `mapearComando()`: normaliza, casa palavras inteiras, trata negação e ambiguidade. 26 casos de teste. |
| Alta | Comando de voz desarmava/destravava sem pedir nada. | Diálogo de confirmação para `DESARMAR` e `DESTRAVAR` por voz. |
| Média | `context` usado sem `mounted` no SnackBar. | Verificação `mounted`. |
| Média | Sem tratamento de desconexão: `_rx` ficava apontando para conexão morta. | Listener de `connectionState`, limpeza de estado e log. |
| Média | `scanResults.firstWhere` podia devolver resultado de uma busca antiga. | `onScanResults` + `Completer` com timeout. |
| Média | Sem `try/catch` em conectar/enviar; duplo clique em "Conectar". | Tratamento de erro e trava `_conectando`. |
| Baixa | Voz podia ficar "ouvindo" para sempre se não houvesse fala. | `onStatus`/`onError` de `speech_to_text`. |
| Baixa | Android: pareamento só começava no primeiro acesso protegido. | `createBond()` explícito. |

## Limitações que **não** foram resolvidas (decisão sua)
1. **Ataque de captura e reenvio atrasado (estilo RollJam):** se um invasor bloquear o rádio, gravar um
   `DESARMAR` válido que o carro nunca recebeu e reproduzi-lo depois, o contador ainda será maior que o último
   aceito e o carro aceita. Solução: *challenge-response* (o carro envia um nonce, o chaveiro assina o nonce).
   Exige uma ida e volta extra (≈ +20 ms).
2. **Consumo no carro:** ESP32 com rádio ligado para ouvir o chaveiro + relé do corte energizado enquanto
   armado = **muitas dezenas de mA a algumas centenas, contínuos**. Meça com um multímetro em série; numa
   bateria de ~50 Ah isso pode significar dias, não semanas, até a bateria não dar mais partida.
3. **Módulo sem energia = carro funciona** (relé NF, de propósito). Quem desligar só o fusível do módulo
   anula o corte de combustível: esconda a fiação/fusível.
4. Parear BLE com **PIN estático** é melhor que nada, mas fraco contra quem fique observando o pareamento.
