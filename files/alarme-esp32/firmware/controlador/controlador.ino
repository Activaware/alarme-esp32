// ============================================================
//  CONTROLADOR VEICULAR — ESP32 (revisado)
//  Core: Arduino-ESP32 3.x  |  Lib extra: TinyGPSPlus
//
//  • Recebe comandos do chaveiro via ESP-NOW (HMAC + contador rolante)
//  • Recebe comandos do celular via BLE (Nordic UART Service + PIN)
//  • Lê sensores (porta, choque, ignição) e dispara sirene/pisca
//  • Lê GPS (NEO-6M) e envia posição ao celular por BLE
//
//  Antes de compilar: python tools/configurar.py  (gera segredos.h)
// ============================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_gap_ble_api.h>
#include <Preferences.h>
#include <TinyGPSPlus.h>
#include "protocolo.h"   // inclui segredos.h (chave, MACs, PIN)

// ---------------- CONFIGURAÇÃO ----------------
#define RELE_ON       HIGH          // use LOW se seu módulo de relé for ativo em nível baixo
#define CHOQUE_ATIVO  HIGH          // nível do DO do SW-420 em vibração (confira no seu módulo!)

// Saídas (módulo de relés 5 V com optoacoplador)
#define PIN_TRAVA     23            // pulso: travar portas
#define PIN_DESTRAVA  22            // pulso: destravar portas
#define PIN_SIRENE    21
#define PIN_PISCA     19            // pisca-alerta
#define PIN_CORTE     18            // corte da bomba (relé NF → fail-safe)
// Entradas
#define PIN_PORTA     32            // LOW = porta aberta (PC817 entre 12 V e GPIO)
#define PIN_CHOQUE    27            // módulo SW-420
#define PIN_IGNICAO   34            // divisor 47k/10k a partir do 12 V da ignição (ADC1)
#define PIN_LED        2
// GPS (UART2)
#define GPS_RX 16                   // ligado ao TX do GPS
#define GPS_TX 17                   // ligado ao RX do GPS

#define TEMPO_SAIDA_MS    5000      // tempo para sair do carro após armar
#define TEMPO_ALARME_MS  30000      // duração de cada ciclo de sirene
#define MAX_DISPAROS         3      // ciclos seguidos sem desarmar (depois fica mudo)
#define INTERVALO_GPS_MS  5000

#define IGN_LIGA_MV       1300      // ≈ 7,4 V na ignição (divisor 47k/10k)
#define IGN_DESLIGA_MV     900      // histerese
#define FILTRO_PORTA_MS     50      // porta precisa ficar aberta por 50 ms
#define FILTRO_IGN_MS      300      // ignição precisa ficar ligada por 300 ms

// ---------------- BLE (Nordic UART Service) ----------------
#define UUID_SERVICO "6E400001-B5A0-F393-E0A9-E50E24DCCA9E"
#define UUID_RX      "6E400002-B5A0-F393-E0A9-E50E24DCCA9E"   // celular -> ESP32
#define UUID_TX      "6E400003-B5A0-F393-E0A9-E50E24DCCA9E"   // ESP32 -> celular

// ---------------- Variáveis globais ----------------
Preferences prefs;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);

Estado estado = ST_DESARMADO;
bool protegido = false;             // alarme "ligado" (persistido; controla o corte de combustível)
uint32_t tEstado = 0;
uint32_t ultimoContador = 0;
uint8_t disparos = 0;

volatile bool temPacote = false;
Pacote pacoteRx;
uint8_t macRx[6];

BLECharacteristic *pTx;
volatile bool bleConectado = false;
QueueHandle_t filaBle;              // comandos BLE (callback roda em outra task)
bool rastrear = true;
uint32_t ultimoGps = 0;

const char *nomeEstado(Estado e) {
  switch (e) {
    case ST_DESARMADO: return "DESARMADO";
    case ST_ARMANDO:   return "ARMANDO";
    case ST_ARMADO:    return "ARMADO";
    case ST_DISPARADO: return "DISPARADO";
    case ST_PANICO:    return "PANICO";
  }
  return "?";
}

// ---------------- BLE: envio ----------------
void bleEnviar(const String &texto) {
  if (!bleConectado) return;
  String msg = texto + "\n";
  // fatia em blocos de 20 bytes (MTU mínimo); o app remonta pelo '\n'
  for (size_t i = 0; i < msg.length(); i += 20) {
    String parte = msg.substring(i, i + 20);
    pTx->setValue((uint8_t *)parte.c_str(), parte.length());
    pTx->notify();
    delay(8);
  }
}

// ---------------- Atuadores ----------------
void pulso(int pino, uint16_t ms) {
  digitalWrite(pino, RELE_ON); delay(ms); digitalWrite(pino, !RELE_ON);
}
void piscaN(int n, int ms = 150) {
  for (int i = 0; i < n; i++) {
    digitalWrite(PIN_PISCA, RELE_ON); delay(ms);
    digitalWrite(PIN_PISCA, !RELE_ON); delay(ms);
  }
}
void bip(int ms) { pulso(PIN_SIRENE, ms); }
void travar()    { pulso(PIN_TRAVA, 400); }
void destravar() { pulso(PIN_DESTRAVA, 400); }

void setProtegido(bool v) {
  protegido = v;
  prefs.putBool("prot", v);
  // Relé NF: energizado = bomba cortada. Sem energia no módulo → carro funciona (fail-safe)
  digitalWrite(PIN_CORTE, v ? RELE_ON : !RELE_ON);
}

void mudarEstado(Estado novo) {
  estado = novo;
  tEstado = millis();
  Serial.printf("Estado -> %s\n", nomeEstado(novo));
  bleEnviar(String("ESTADO:") + nomeEstado(novo));
}

// ---------------- Sensores (com filtro anti-ruído) ----------------
struct Filtro { bool bruto = false; uint32_t desde = 0; };

// Sobe só depois de `ms` estável; desce imediatamente.
bool persistente(Filtro &f, bool leitura, uint32_t ms) {
  if (leitura != f.bruto) { f.bruto = leitura; f.desde = millis(); }
  return f.bruto && (millis() - f.desde >= ms);
}

Filtro fPorta, fIgn;
bool portaAbertaF = false, ignicaoLigadaF = false, ignBruta = false;

void lerSensores() {   // chamar a cada volta do loop
  portaAbertaF = persistente(fPorta, digitalRead(PIN_PORTA) == LOW, FILTRO_PORTA_MS);
  uint32_t mv = analogReadMilliVolts(PIN_IGNICAO);
  if (mv > IGN_LIGA_MV) ignBruta = true;
  else if (mv < IGN_DESLIGA_MV) ignBruta = false;
  ignicaoLigadaF = persistente(fIgn, ignBruta, FILTRO_IGN_MS);
}
bool portaAberta()   { return portaAbertaF; }
bool ignicaoLigada() { return ignicaoLigadaF; }

bool detectouChoque() {
  static uint32_t janela = 0, ultimoPulso = 0;
  static uint8_t n = 0;
  static bool anterior = false;
  uint32_t agora = millis();
  if (agora - janela > 1000) { janela = agora; n = 0; }
  bool atual = (digitalRead(PIN_CHOQUE) == CHOQUE_ATIVO);
  if (atual && !anterior && agora - ultimoPulso > 40) { n++; ultimoPulso = agora; }  // 40 ms: ignora repique
  anterior = atual;
  if (n >= 2) { n = 0; return true; }     // 2 pancadas em 1 s
  return false;
}

// ---------------- Ações do alarme ----------------
void armar() {
  if (estado != ST_DESARMADO) { bleEnviar("ERRO:JA_ARMADO"); return; }
  if (ignicaoLigada()) { bleEnviar("ERRO:IGNICAO_LIGADA"); piscaN(3, 80); return; }
  if (portaAberta())   { bleEnviar("ERRO:PORTA_ABERTA");   piscaN(3, 80); return; }
  disparos = 0;
  mudarEstado(ST_ARMANDO);          // atualiza estado ANTES dos atuadores (resposta rápida)
  setProtegido(true);               // só chega aqui com a ignição desligada
}
void armarAtuadores()    { travar(); piscaN(1); }

void desarmar() {
  disparos = 0;
  setProtegido(false);
  mudarEstado(ST_DESARMADO);
}
void desarmarAtuadores() { destravar(); piscaN(2); }

void panico() {
  if (estado == ST_PANICO) mudarEstado(protegido ? ST_ARMADO : ST_DESARMADO);
  else mudarEstado(ST_PANICO);
}

void buscar() { piscaN(3, 150); bip(150); }

void disparar(const char *motivo) {
  if (disparos >= MAX_DISPAROS) return;   // já tocou o bastante: fica mudo até desarmar
  disparos++;
  mudarEstado(ST_DISPARADO);
  bleEnviar(String("ALERTA:") + motivo);
  rastrear = true;
}

// ---------------- GPS ----------------
void enviarGps() {
  if (gps.location.isValid() && gps.location.age() < 5000) {
    bleEnviar("GPS:" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6) +
              "," + String(gps.speed.kmph(), 0) + "," + String(gps.satellites.value()));
  } else {
    bleEnviar("GPS:SEM_FIX");
  }
}

// ---------------- Comandos ----------------
// Muda só o estado (rápido). Os atuadores bloqueantes rodam depois da resposta.
void executarComandoRadio(uint8_t cmd) {
  switch (cmd) {
    case CMD_ARMAR:    if (estado == ST_DESARMADO) armar(); break;
    case CMD_DESARMAR: desarmar(); break;
    case CMD_PANICO:   panico(); break;
    case CMD_BUSCAR:   break;
  }
}

void tratarComandoBle(String c) {
  c.trim(); c.toUpperCase();
  Serial.println("BLE <- " + c);
  if (c == "ARMAR")            { armar(); if (estado == ST_ARMANDO) armarAtuadores(); }
  else if (c == "DESARMAR")    { desarmar(); desarmarAtuadores(); }
  else if (c == "TRAVAR")      { travar();    bleEnviar("OK:TRAVADO"); }
  else if (c == "DESTRAVAR")   { destravar(); bleEnviar("OK:DESTRAVADO"); }
  else if (c == "PANICO")      { panico(); }
  else if (c == "BUSCAR")      { buscar(); bleEnviar("OK:BUSCAR"); }
  else if (c == "STATUS")      { bleEnviar(String("ESTADO:") + nomeEstado(estado)); }
  else if (c == "LOCALIZAR")   { enviarGps(); }
  else if (c == "RASTREAR:ON") { rastrear = true;  bleEnviar("OK:RASTREAR_ON"); }
  else if (c == "RASTREAR:OFF"){ rastrear = false; bleEnviar("OK:RASTREAR_OFF"); }
  else                         { bleEnviar("ERRO:COMANDO_INVALIDO"); }
}

// ---------------- ESP-NOW ----------------
void aoReceberEspNow(const esp_now_recv_info_t *info, const uint8_t *dados, int tam) {
  if (tam != sizeof(Pacote) || temPacote) return;
  memcpy((void *)&pacoteRx, dados, sizeof(Pacote));
  memcpy(macRx, info->src_addr, 6);
  temPacote = true;
}

void responderChaveiro(uint32_t contador) {
  Pacote r = {PKT_RESPOSTA, (uint8_t)estado, contador, {0}};
  assinar(r);
  esp_now_send(MAC_TRANSMISSOR, (uint8_t *)&r, sizeof(r));
}

void processarPacote() {
  Pacote p;
  uint8_t mac[6];
  memcpy(&p, (const void *)&pacoteRx, sizeof(p));
  memcpy(mac, macRx, 6);
  temPacote = false;

  if (memcmp(mac, MAC_TRANSMISSOR, 6) != 0) return;       // só o chaveiro pareado
  if (p.tipo != PKT_COMANDO || !verificar(p)) return;     // assinatura inválida

  if (p.contador == ultimoContador) { responderChaveiro(p.contador); return; }  // retransmissão
  if (p.contador < ultimoContador) return;                // replay → ignora

  ultimoContador = p.contador;
  prefs.putUInt("cnt", ultimoContador);

  executarComandoRadio(p.valor);          // muda o estado (rápido)
  responderChaveiro(p.contador);          // confirma ao chaveiro (com o estado resultante)
  // Atuadores (bloqueantes, ~1 s) depois da resposta:
  if (p.valor == CMD_ARMAR && estado == ST_ARMANDO) armarAtuadores();
  else if (p.valor == CMD_DESARMAR)                 desarmarAtuadores();
  else if (p.valor == CMD_BUSCAR)                   buscar();
}

// ---------------- BLE ----------------
class CbServidor : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { bleConectado = true; }
  void onDisconnect(BLEServer *s) override {
    bleConectado = false;
    BLEDevice::startAdvertising();
  }
};

class CbRx : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    auto v = c->getValue();                 // String (core 3.x) ou std::string (2.x)
    char buf[24];
    size_t n = v.length() < sizeof(buf) - 1 ? v.length() : sizeof(buf) - 1;
    memcpy(buf, v.c_str(), n);
    buf[n] = 0;
    xQueueSend(filaBle, buf, 0);            // não perde comandos seguidos
  }
};

void iniciarBle() {
  BLEDevice::init("Alarme-ESP32");
  BLEDevice::setMTU(185);

  // Segurança: pareamento com PIN fixo (o celular pede os 6 dígitos)
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
  BLESecurity *seg = new BLESecurity();
  seg->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  seg->setCapability(ESP_IO_CAP_OUT);
  seg->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  uint32_t passkey = PIN_BLE_PASSKEY;
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(passkey));

  BLEServer *servidor = BLEDevice::createServer();
  servidor->setCallbacks(new CbServidor());
  BLEService *servico = servidor->createService(UUID_SERVICO);

  pTx = servico->createCharacteristic(UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTx->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  // O CCCD (0x2902) também exige pareamento; senão um celular não pareado
  // poderia assinar as notificações (estado/GPS). Se sua versão do core criar o
  // CCCD sozinha e der erro de compilação/duplicidade, remova estas 3 linhas.
  BLE2902 *cccd = new BLE2902();
  cccd->setAccessPermissions((esp_gatt_perm_t)(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM));
  pTx->addDescriptor(cccd);

  BLECharacteristic *pRx = servico->createCharacteristic(
      UUID_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  pRx->setCallbacks(new CbRx());

  servico->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(UUID_SERVICO);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void iniciarEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);
  Serial.println("MAC do controlador: " + WiFi.macAddress());
  if (esp_now_init() != ESP_OK) { Serial.println("Falha ESP-NOW"); return; }
  esp_now_register_recv_cb(aoReceberEspNow);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_TRANSMISSOR, 6);
  peer.channel = CANAL_ESPNOW;
  peer.encrypt = false;                   // autenticidade garantida pelo HMAC
  esp_now_add_peer(&peer);
}

// ---------------- Máquina de estados ----------------
void maquinaEstados() {
  switch (estado) {
    case ST_ARMANDO:
      if (millis() - tEstado > TEMPO_SAIDA_MS) mudarEstado(ST_ARMADO);
      break;
    case ST_ARMADO:
      if (portaAberta())         disparar("PORTA");
      else if (detectouChoque()) disparar("CHOQUE");
      else if (ignicaoLigada())  disparar("IGNICAO");
      break;
    case ST_DISPARADO:
    case ST_PANICO:
      if (millis() - tEstado > TEMPO_ALARME_MS)
        mudarEstado(protegido ? ST_ARMADO : ST_DESARMADO);
      break;
    default: break;
  }
}

void atualizarSaidas() {
  bool alarme = (estado == ST_DISPARADO || estado == ST_PANICO);
  if (alarme) {
    bool ligado = (millis() / 400) % 2;
    digitalWrite(PIN_SIRENE, ligado ? RELE_ON : !RELE_ON);
    digitalWrite(PIN_PISCA,  ligado ? RELE_ON : !RELE_ON);
  } else {
    digitalWrite(PIN_SIRENE, !RELE_ON);
    digitalWrite(PIN_PISCA,  !RELE_ON);
  }
  // LED de status: pisca curto a cada 2 s quando armado
  digitalWrite(PIN_LED, (estado == ST_ARMADO && (millis() % 2000) < 100) ? HIGH : LOW);
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);

  const int saidas[] = {PIN_TRAVA, PIN_DESTRAVA, PIN_SIRENE, PIN_PISCA, PIN_CORTE};
  for (int p : saidas) { pinMode(p, OUTPUT); digitalWrite(p, !RELE_ON); }
  pinMode(PIN_PORTA, INPUT_PULLUP);
  pinMode(PIN_CHOQUE, INPUT);
  pinMode(PIN_LED, OUTPUT);
  analogSetPinAttenuation(PIN_IGNICAO, ADC_11db);

  prefs.begin("alarme", false);
  ultimoContador = prefs.getUInt("cnt", 0);
  protegido = prefs.getBool("prot", false);
  digitalWrite(PIN_CORTE, protegido ? RELE_ON : !RELE_ON);
  estado = protegido ? ST_ARMADO : ST_DESARMADO;   // volta armado após queda de energia

  GPSSerial.setRxBufferSize(1024);
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  filaBle = xQueueCreate(4, 24);
  iniciarEspNow();   // primeiro Wi-Fi/ESP-NOW...
  iniciarBle();      // ...depois BLE (coexistência)
  Serial.println("Controlador pronto.");
}

void loop() {
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  lerSensores();
  if (temPacote) processarPacote();
  char cmd[24];
  while (xQueueReceive(filaBle, cmd, 0) == pdTRUE) tratarComandoBle(String(cmd));

  maquinaEstados();
  atualizarSaidas();

  if (rastrear && bleConectado && millis() - ultimoGps >= INTERVALO_GPS_MS) {
    ultimoGps = millis();
    enviarGps();
  }
}
