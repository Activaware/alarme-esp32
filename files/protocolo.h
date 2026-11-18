#pragma once
// ============================================================
//  protocolo.h  —  IDÊNTICO nos dois ESP32
//  Fonte da verdade: firmware/common/. Não edite as cópias em
//  controlador/ e transmissor/: rode `python tools/configurar.py`.
// ============================================================
#include <Arduino.h>
#include <stddef.h>
#include "mbedtls/md.h"

// CHAVE_SECRETA, MAC_CONTROLADOR, MAC_TRANSMISSOR e PIN_BLE_PASSKEY
// vêm de segredos.h (gerado por tools/configurar.py, fora do git).
#if __has_include("segredos.h")
  #include "segredos.h"
#else
  #error "segredos.h não existe. Rode: python tools/configurar.py"
#endif

// Os dois ESP32 precisam estar no mesmo canal Wi-Fi
#define CANAL_ESPNOW 1

enum Comando : uint8_t {
  CMD_NENHUM = 0,
  CMD_ARMAR = 1,
  CMD_DESARMAR = 2,
  CMD_PANICO = 3,
  CMD_BUSCAR = 4          // "onde está meu carro?" (pisca + bip)
};

enum Estado : uint8_t {
  ST_DESARMADO = 0,
  ST_ARMANDO = 1,         // tempo de saída (sensores ignorados)
  ST_ARMADO = 2,
  ST_DISPARADO = 3,
  ST_PANICO = 4
};

enum TipoPacote : uint8_t { PKT_COMANDO = 1, PKT_RESPOSTA = 2 };

struct __attribute__((packed)) Pacote {
  uint8_t  tipo;          // PKT_COMANDO ou PKT_RESPOSTA
  uint8_t  valor;         // comando (ida) ou estado atual (volta)
  uint32_t contador;      // contador rolante (anti-replay)
  uint8_t  hmac[8];       // HMAC-SHA256 truncado em 8 bytes
};

inline void calcularAssinatura(const Pacote &p, uint8_t saida[8]) {
  uint8_t completo[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, CHAVE_SECRETA, sizeof(CHAVE_SECRETA),
                  (const uint8_t *)&p, offsetof(Pacote, hmac), completo);
  memcpy(saida, completo, 8);
}

inline void assinar(Pacote &p) { calcularAssinatura(p, p.hmac); }

inline bool verificar(const Pacote &p) {
  uint8_t esperado[8];
  calcularAssinatura(p, esperado);
  uint8_t diff = 0;                       // comparação em tempo constante
  for (int i = 0; i < 8; i++) diff |= esperado[i] ^ p.hmac[i];
  return diff == 0;
}
