// ============================================================
//  CHAVEIRO (TRANSMISSOR) — ESP32 + ESP-NOW (revisado)
//  Core: Arduino-ESP32 3.x
//
//  Fica em deep sleep. Apertar um botão acorda o ESP32, envia o
//  comando assinado (HMAC + contador rolante), espera a confirmação
//  do controlador e volta a dormir.
//
//  Botões (entre o pino e o 3V3, SEM resistor externo — usa o
//  pull-down interno RTC):
//    GPIO25 = ARMAR | GPIO26 = DESARMAR | GPIO27 = PÂNICO
//
//  LED:  1 piscada longa  = comando aceito
//        2 piscadas médias = carro recebeu, mas recusou (porta aberta,
//                            ignição ligada...)
//        3 piscadas rápidas = sem resposta (fora de alcance)
//
//  Antes de compilar: python tools/configurar.py  (gera segredos.h)
// ============================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <Preferences.h>
#include "protocolo.h"   // inclui segredos.h (chave e MACs)

#define BTN_ARMAR     25
#define BTN_DESARMAR  26
#define BTN_PANICO    27
#define PIN_LED        2

const int PINOS_BOTOES[] = {BTN_ARMAR, BTN_DESARMAR, BTN_PANICO};
const uint64_t MASCARA_BOTOES =
    (1ULL << BTN_ARMAR) | (1ULL << BTN_DESARMAR) | (1ULL << BTN_PANICO);

volatile bool confirmado = false;
volatile uint8_t estadoRecebido = 0;
uint32_t contadorEnviado = 0;

// Resposta do controlador
void aoReceber(const esp_now_recv_info_t *info, const uint8_t *dados, int tam) {
  if (tam != sizeof(Pacote)) return;
  Pacote p;
  memcpy(&p, dados, sizeof(p));
  if (p.tipo != PKT_RESPOSTA || !verificar(p) || p.contador != contadorEnviado) return;
  estadoRecebido = p.valor;
  confirmado = true;
}

void dormir() {
  esp_sleep_enable_ext1_wakeup(MASCARA_BOTOES, ESP_EXT1_WAKEUP_ANY_HIGH);
  // No IDF 5.x (core 3.x) o ext1 reinicializa os pinos RTC: os pull-downs
  // precisam ser configurados DEPOIS, senão as entradas flutuam e o chip acorda sozinho.
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  for (int pino : PINOS_BOTOES) {
    rtc_gpio_pullup_dis((gpio_num_t)pino);
    rtc_gpio_pulldown_en((gpio_num_t)pino);
  }
  esp_deep_sleep_start();
}

void piscarLed(int vezes, int ms) {
  for (int i = 0; i < vezes; i++) {
    digitalWrite(PIN_LED, HIGH); delay(ms);
    digitalWrite(PIN_LED, LOW);  delay(ms);
  }
}

// Espera TODOS os botões soltos (senão o ext1 acordaria de novo e repetiria o comando)
void aguardarSoltar() {
  for (int pino : PINOS_BOTOES) pinMode(pino, INPUT_PULLDOWN);
  uint32_t t0 = millis();
  while (millis() - t0 < 15000) {
    bool algum = false;
    for (int pino : PINOS_BOTOES) if (digitalRead(pino) == HIGH) algum = true;
    if (!algum) break;
    delay(10);
  }
  delay(30);   // antirrepique
}

// O carro realmente fez o que foi pedido?
bool atendido(uint8_t cmd, uint8_t st) {
  switch (cmd) {
    case CMD_ARMAR:    return st == ST_ARMANDO || st == ST_ARMADO;
    case CMD_DESARMAR: return st == ST_DESARMADO;
    default:           return true;     // pânico alterna; basta ter recebido
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);

  esp_sleep_wakeup_cause_t causa = esp_sleep_get_wakeup_cause();

  // Primeira energização (não veio de deep sleep): mostra o MAC e dorme
  if (causa == ESP_SLEEP_WAKEUP_UNDEFINED) {
    WiFi.mode(WIFI_STA);
    Serial.println("MAC do chaveiro: " + WiFi.macAddress());
    Serial.println("Dormindo em 5 s...");
    delay(5000);
    dormir();
  }
  if (causa != ESP_SLEEP_WAKEUP_EXT1) dormir();

  // Qual botão acordou o chip? (pânico tem prioridade)
  uint64_t status = esp_sleep_get_ext1_wakeup_status();
  uint8_t cmd = CMD_NENHUM;
  if (status & (1ULL << BTN_PANICO))         cmd = CMD_PANICO;
  else if (status & (1ULL << BTN_DESARMAR))  cmd = CMD_DESARMAR;
  else if (status & (1ULL << BTN_ARMAR))     cmd = CMD_ARMAR;
  if (cmd == CMD_NENHUM) dormir();

  // Contador rolante persistente (salvo ANTES de transmitir)
  Preferences prefs;
  prefs.begin("chave", false);
  contadorEnviado = prefs.getUInt("cnt", 0) + 1;
  prefs.putUInt("cnt", contadorEnviado);
  prefs.end();

  Pacote pkt = {PKT_COMANDO, cmd, contadorEnviado, {0}};
  assinar(pkt);

  // ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) { piscarLed(10, 50); dormir(); }
  esp_now_register_recv_cb(aoReceber);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_CONTROLADOR, 6);
  peer.channel = CANAL_ESPNOW;
  peer.encrypt = false;             // autenticidade garantida pelo HMAC
  esp_now_add_peer(&peer);

  // Envia até 6 vezes (a cada 300 ms) até receber a confirmação
  digitalWrite(PIN_LED, HIGH);
  for (int i = 0; i < 6 && !confirmado; i++) {
    esp_now_send(MAC_CONTROLADOR, (uint8_t *)&pkt, sizeof(pkt));
    uint32_t t0 = millis();
    while (!confirmado && millis() - t0 < 300) delay(5);
  }
  digitalWrite(PIN_LED, LOW);

  if (!confirmado) {
    Serial.println("Sem resposta (fora de alcance?)");
    piscarLed(3, 80);
  } else if (atendido(cmd, estadoRecebido)) {
    Serial.printf("OK. Estado do carro: %u\n", estadoRecebido);
    piscarLed(1, 400);
  } else {
    Serial.printf("Recebido, mas recusado. Estado do carro: %u\n", estadoRecebido);
    piscarLed(2, 150);
  }

  aguardarSoltar();
  dormir();
}

void loop() {}
