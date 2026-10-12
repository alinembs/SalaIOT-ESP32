/*
 * ESP32 + HC-SR04 (presenca) + DS18B20 (temperatura) + LED + RELE, tudo via MQTT.
 *
 * LIGACAO
 *   HC-SR04   VCC -> 5V (VIN) | GND | TRIG -> GPIO5 | ECHO -> divisor 1k/2k -> GPIO18
 *   DS18B20   VCC -> 3V3 | GND | DATA -> GPIO4 e resistor de 4,7k entre DATA e 3V3
 *   LED       GPIO2 (LED da placa) ou externo: GPIO -> 220 ohm -> LED -> GND
 *   RELE      IN -> GPIO13 | VCC conforme o modulo (geralmente 5V) | GND comum com o ESP32
 *
 * TOPICOS (BASE = "casa/sala1")
 *   Publica:
 *     BASE/dados        JSON a cada 1 s: distancia, presenca, temperatura e estado das saidas
 *     BASE/presenca     "1" / "0"  (retido; quando muda e a cada 1 s)
 *     BASE/led/estado   "ON" / "OFF" (retido)
 *     BASE/rele/estado  "ON" / "OFF" (retido)
 *     BASE/status       "online" / "offline" (offline = ultima vontade do broker)
 *   Escuta:
 *     BASE/led/set      on | off | toggle | 1 | 0
 *     BASE/rele/set     on | off | toggle | 1 | 0
 *     BASE/cmd          calibrar   (refaz a base de presenca; deixe o ambiente vazio)
 *
 * IMPORTANTE: publique comandos SEM retain (-r). Um comando retido no broker seria
 * reenviado a cada reconexao e ligaria o rele sozinho depois de uma queda de energia.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "presenca_hcsr04.h"
#include "temperatura_ds18b20.h"
#include "saidas.h"

// ---------------- AJUSTE AQUI ----------------
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "192.168.x.x";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_ID = "esp32-sala1";

#define BASE "casa/sala1"
const uint8_t PIN_DS18B20 = 4;
const uint8_t PIN_LED = 2;
const uint8_t PIN_RELE = 13;
const bool RELE_ATIVO_BAIXO = false;      // maioria dos modulos de rele: IN=LOW liga
// ----------------------------------------------

#define T_DADOS       BASE "/dados"
#define T_PRESENCA    BASE "/presenca"
#define T_STATUS      BASE "/status"
#define T_LED_SET     BASE "/led/set"
#define T_LED_ESTADO  BASE "/led/estado"
#define T_RELE_SET    BASE "/rele/set"
#define T_RELE_ESTADO BASE "/rele/estado"
#define T_CMD         BASE "/cmd"

WiFiClient wifi;
PubSubClient mqtt(wifi);
PresencaHCSR04 presenca;                 // HC-SR04: pinos 5/18 e calibracao no config
TemperaturaDS18B20 temperatura(PIN_DS18B20, 5000);
Saida led(PIN_LED, false);
Saida rele(PIN_RELE, RELE_ATIVO_BAIXO);

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------
static void num(char* out, size_t n, float v, int dec = 1) {   // NAN vira "null" (JSON valido)
  if (isnan(v)) strlcpy(out, "null", n);
  else snprintf(out, n, "%.*f", dec, v);
}

static void publicaEstado(Saida& s, const char* topico) {
  if (mqtt.connected()) mqtt.publish(topico, s.estado() ? "ON" : "OFF", true);
}

// ------------------------------------------------------------------
// MQTT: comandos recebidos. Roda dentro de mqtt.loop(), ou seja, na task do loop().
// So essa task mexe nas saidas, entao nao precisa de mutex.
// ------------------------------------------------------------------
void aoReceberMqtt(char* topico, uint8_t* payload, unsigned int tam) {
  char buf[24];
  unsigned int n = tam < sizeof(buf) - 1 ? tam : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = 0;

  char* msg = buf;                                   // remove espacos e quebras de linha
  while (*msg == ' ' || *msg == '\r' || *msg == '\n') msg++;
  size_t len = strlen(msg);
  while (len && (msg[len - 1] == ' ' || msg[len - 1] == '\r' || msg[len - 1] == '\n')) msg[--len] = 0;

  Serial.printf("MQTT <- %s : %s\n", topico, msg);

  if (!strcmp(topico, T_LED_SET)) {
    if (led.comando(msg)) publicaEstado(led, T_LED_ESTADO);
    else Serial.println("  comando invalido para o LED");
  } else if (!strcmp(topico, T_RELE_SET)) {
    if (rele.comando(msg)) publicaEstado(rele, T_RELE_ESTADO);
    else Serial.println("  comando invalido para o rele");
  } else if (!strcmp(topico, T_CMD)) {
    if (!strcasecmp(msg, "calibrar")) presenca.recalibrarBase();   // so sinaliza; a task faz o trabalho
    else Serial.println("  comando desconhecido");
  }
}

// Reconecta Wi-Fi e MQTT sem travar (uma tentativa a cada 5 s)
void manterConexoes() {
  static uint32_t ultimaTentativa = 0;
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) return;
  if (millis() - ultimaTentativa < 5000) return;
  ultimaTentativa = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi: reconectando...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return;
  }

  Serial.println("MQTT: conectando...");
  if (mqtt.connect(MQTT_ID, nullptr, nullptr, T_STATUS, 1, true, "offline")) {
    mqtt.publish(T_STATUS, "online", true);
    mqtt.subscribe(T_LED_SET);
    mqtt.subscribe(T_RELE_SET);
    mqtt.subscribe(T_CMD);
    publicaEstado(led, T_LED_ESTADO);                // o broker fica sabendo o estado real
    publicaEstado(rele, T_RELE_ESTADO);
    Serial.println("MQTT: conectado");
  } else {
    Serial.printf("MQTT: falhou (rc=%d)\n", mqtt.state());
  }
}

// ------------------------------------------------------------------
// setup / loop
// ------------------------------------------------------------------
void setup() {
  rele.begin();                  // primeiro: rele e LED ja iniciam DESLIGADOS
  led.begin();

  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32: presenca + temperatura + LED + rele via MQTT");
  Serial.println("Deixe o ambiente VAZIO no boot (a base de presenca e calibrada agora).");

  if (!temperatura.begin()) Serial.println("ERRO: task de temperatura nao iniciou");

  // Espera a 1a temperatura (ate 2 s) para a base de presenca ja usar o valor real
  LeituraTemp lt = {NAN, false, 0};
  uint32_t t0 = millis();
  while (!temperatura.ultima(lt) && millis() - t0 < 2000) delay(50);
  if (lt.ok) presenca.setTemperatura(lt.tempC);
  Serial.printf("Temperatura inicial: %s\n", lt.ok ? String(lt.tempC, 1).c_str() : "indisponivel (usando 25C)");

  if (!presenca.begin()) Serial.println("ERRO: task de presenca nao iniciou");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(aoReceberMqtt);
}

void loop() {
  manterConexoes();
  if (mqtt.connected()) mqtt.loop();

  // 1) Mudanca de presenca -> publica na hora
  EventoPresenca e;
  while (presenca.pegarEvento(e)) {
    Serial.printf("EVENTO: %s (d=%.1f cm)\n", e.presente ? "PRESENTE" : "AUSENTE", e.distanciaCm);
    if (mqtt.connected()) mqtt.publish(T_PRESENCA, e.presente ? "1" : "0", true);
  }

  // 2) A cada 1 s: atualiza a temperatura usada na compensacao e publica tudo junto
  static uint32_t ultimoPub = 0;
  if (millis() - ultimoPub >= 1000) {
    ultimoPub = millis();

    LeituraTemp lt;
    bool temLt = temperatura.ultima(lt);
    if (temLt && lt.ok) presenca.setTemperatura(lt.tempC);

    LeituraPresenca lp;
    if (presenca.ultimaLeitura(lp)) {
      char d[16], base[16], t[16], json[200];
      num(d, sizeof(d), lp.distanciaCm);
      num(base, sizeof(base), lp.baseCm);
      num(t, sizeof(t), (temLt && lt.ok) ? lt.tempC : NAN);

      snprintf(json, sizeof(json),
               "{\"d\":%s,\"base\":%s,\"p\":%d,\"ok\":%d,\"t\":%s,\"tok\":%d,\"led\":%d,\"rele\":%d,\"up\":%lu}",
               d, base, lp.presente ? 1 : 0, lp.sensorOk ? 1 : 0, t,
               (temLt && lt.ok) ? 1 : 0, led.estado() ? 1 : 0, rele.estado() ? 1 : 0,
               (unsigned long)(millis() / 1000));
      Serial.println(json);

      if (mqtt.connected()) {
        mqtt.publish(T_DADOS, json);
        mqtt.publish(T_PRESENCA, lp.presente ? "1" : "0", true);   // corrige evento perdido
      }
    }
  }
}
