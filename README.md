# Alarme Automotivo com 2 × ESP32

Projeto de alarme automotivo com **chaveiro** (ESP32 em deep sleep, ESP-NOW), **controlador veicular** (ESP32 com relés, sensores e GPS) e **app Flutter** (BLE, comandos de voz em pt-BR e mapa).

> ⚠️ Projeto de bancada/hobby, não testado em veículo. Mexer em travas, bomba de combustível e fiação original pode afetar segurança, garantia e seguro.

## Estrutura

- `Alarme_Esp32/` — documentação principal (README, conferência de pinos, revisão de firmware, esquemáticos)
- `files/` — código-fonte
  - `files/alarme-esp32/firmware/common/protocolo.h` — fonte da verdade do protocolo
  - `files/alarme-esp32/firmware/controlador/` — sketch do módulo do carro
  - `files/alarme-esp32/firmware/transmissor/` — sketch do chaveiro
  - `files/alarme-esp32/app_flutter/` — app Flutter
  - `files/alarme-esp32/tools/configurar.py` — gera segredos e sincroniza protocolo

## Funcionalidades

- Chaveiro: armar, desarmar e pânico; bateria dura meses (deep sleep); LED de confirmação.
- Controlador: sensores de porta, choque e ignição; sirene, pisca-alerta, trava/destrava e corte da bomba (relé NF, fail-safe).
- Celular: botões, comando de voz, estado em tempo real, alerta de disparo, posição GPS e abrir no mapa.
- Estado armado persiste em flash (volta armado após queda de energia).

## Segurança

- Pacotes ESP-NOW assinados com HMAC-SHA256 + contador rolante (anti-replay).
- Controlador aceita só o MAC do chaveiro pareado.
- BLE com pareamento por PIN e características criptografadas.
- Chave e PIN aleatórios por instalação, fora do git (`tools/configurar.py`).

## Como começar

Veja [`Alarme_Esp32/README.md`](Alarme_Esp32/README.md) para o passo a passo completo (ferramentas, segredos, pareamento, app, testes de bancada).

## Licença

Projeto pessoal/hobby. Use por sua conta e risco.