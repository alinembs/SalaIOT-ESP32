/*
 * presenca_hcsr04.h
 * Deteccao de presenca com HC-SR04 rodando em uma TASK do FreeRTOS (ESP32).
 *
 * A task le o sensor a cada 'periodoMs', filtra, decide "presente / ausente"
 * e entrega o resultado por FILAS (queues). Quem quiser (loop, MQTT, display)
 * le das filas, sem nunca tocar no sensor nem em variaveis da task.
 *
 *   filaEventos : so quando o estado MUDA (presente <-> ausente)   -> pegarEvento()
 *   filaUltima  : sempre a leitura mais recente (tamanho 1)        -> ultimaLeitura()
 *
 */

#ifndef PRESENCA_HCSR04_H
#define PRESENCA_HCSR04_H

#include <Arduino.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ------------------------------------------------------------------
// Configuracao (valores iniciais = os da sua calibracao do HC-SR04)
// ------------------------------------------------------------------
struct PresencaConfig {
  uint8_t trig = 5;
  uint8_t echo = 18;
  float ganho = 1.02251f;        // calibracao (comando 'k' do teste)
  float offset = -0.3116f;
  float minCm = 2.0f;            // faixa valida do HC-SR04
  float maxCm = 400.0f;
  float margemEntrar = 20.0f;    // presente se distancia < base - margemEntrar
  float margemSair = 10.0f;      // ausente  se distancia > base - margemSair (histerese)
  uint8_t leiturasEntrar = 3;    // leituras seguidas para confirmar presenca  (~0,2 s)
  uint8_t leiturasSair = 25;     // leituras seguidas para confirmar ausencia  (~1,5 s)
  uint32_t periodoMs = 60;       // tempo entre disparos (>= 50 ms)
};

// Dados que saem da task (copiados pela fila, entao sao seguros entre tasks)
struct LeituraPresenca {
  float distanciaCm;   // filtrada; NAN se a ultima leitura falhou
  float baseCm;        // distancia do ambiente vazio
  bool presente;
  bool sensorOk;       // false se muitas falhas seguidas ou base nao calibrada
  uint32_t tMs;
};

struct EventoPresenca {
  bool presente;
  float distanciaCm;
  uint32_t tMs;
};

// ------------------------------------------------------------------
// Classe
// ------------------------------------------------------------------
class PresencaHCSR04 {
 public:
  explicit PresencaHCSR04(const PresencaConfig& cfg = PresencaConfig()) : _cfg(cfg) {}

  // Cria as filas e a task. Chame no setup(). Retorna false se faltou memoria.
  bool begin(UBaseType_t prioridade = 2, BaseType_t core = 1) {
    pinMode(_cfg.trig, OUTPUT);
    pinMode(_cfg.echo, INPUT);
    digitalWrite(_cfg.trig, LOW);

    _filaEventos = xQueueCreate(8, sizeof(EventoPresenca));
    _filaUltima  = xQueueCreate(1, sizeof(LeituraPresenca));
    if (!_filaEventos || !_filaUltima) return false;

    BaseType_t ok = xTaskCreatePinnedToCore(
        tarefaEstatica,   // funcao da task
        "presenca",       // nome (aparece em debug)
        4096,             // pilha em bytes
        this,             // argumento: ponteiro para esta instancia
        prioridade,       // maior numero = maior prioridade (loop() roda com 1)
        &_tarefa,
        core);            // 1 = mesmo nucleo do loop(); o Wi-Fi roda no 0
    return ok == pdPASS;
  }

  // ---- API para outras tasks / loop() ----
  // Pega um evento de mudanca de estado, se houver. 'espera' em ticks (0 = nao espera).
  bool pegarEvento(EventoPresenca& e, TickType_t espera = 0) {
    return xQueueReceive(_filaEventos, &e, espera) == pdTRUE;
  }
  // Copia a leitura mais recente SEM remove-la da fila (varios leitores podem ler).
  bool ultimaLeitura(LeituraPresenca& l) {
    return xQueuePeek(_filaUltima, &l, 0) == pdTRUE;
  }
  // Pede para a task refazer a base (faca com o ambiente vazio ~2 s).
  void recalibrarBase() { _pedirRecalibrar = true; }
  // Temperatura para compensar a velocidade do som (ex.: vinda do DS18B20).
  void setTemperatura(float c) { _tempC = c; }

 private:
  // Mediana de 3: corta picos isolados sem atrasar a deteccao
  struct Mediana3 {
    float b[3];
    uint8_t n = 0, i = 0;
    void reset() { n = 0; i = 0; }
    float add(float x) {
      b[i] = x; i = (i + 1) % 3; if (n < 3) n++;
      if (n == 1) return b[0];
      if (n == 2) return (b[0] + b[1]) / 2.0f;
      float a = b[0], m = b[1], c = b[2];
      if ((a <= m && m <= c) || (c <= m && m <= a)) return m;
      if ((m <= a && a <= c) || (c <= a && a <= m)) return a;
      return c;
    }
  };

  PresencaConfig _cfg;
  TaskHandle_t _tarefa = nullptr;
  QueueHandle_t _filaEventos = nullptr;
  QueueHandle_t _filaUltima = nullptr;

  // Compartilhado entre tasks: so flags/valores de 32 bits, marcados volatile
  volatile bool _pedirRecalibrar = false;
  volatile float _tempC = 25.0f;

  // Estado PRIVADO da task (so a task mexe nisso)
  Mediana3 _filtro;
  float _base = NAN;
  bool _baseValida = false;
  bool _presente = false;
  bool _ok = true;
  uint8_t _contEntra = 0, _contSai = 0;
  uint16_t _falhas = 0;

  static void tarefaEstatica(void* arg) {
    static_cast<PresencaHCSR04*>(arg)->tarefa();
  }

  float cmPorUs() const { return (331.3f + 0.606f * _tempC) / 10000.0f; }

  float lerBruto() {
    digitalWrite(_cfg.trig, LOW);
    delayMicroseconds(4);
    digitalWrite(_cfg.trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(_cfg.trig, LOW);
    unsigned long d = pulseIn(_cfg.echo, HIGH, 30000UL);  // timeout 30 ms
    if (d == 0) return NAN;
    float cm = d * cmPorUs() / 2.0f;
    if (cm < _cfg.minCm || cm > _cfg.maxCm) return NAN;
    return cm;
  }

  float lerCal() {
    float b = lerBruto();
    return isnan(b) ? NAN : b * _cfg.ganho + _cfg.offset;
  }

  void enviaEvento(float d) {
    EventoPresenca e{_presente, d, millis()};
    xQueueSend(_filaEventos, &e, 0);   // 0 = nao bloqueia; se a fila encher, descarta
  }

  void publicaUltima(float d) {
    LeituraPresenca l{d, _base, _presente, (bool)(_ok && _baseValida), millis()};
    xQueueOverwrite(_filaUltima, &l);  // sobrescreve: a fila sempre tem o valor mais novo
  }

  void calibrarBaseInterno() {
    bool tinhaPresenca = _presente;
    float v[25];
    uint8_t n = 0;
    for (uint8_t k = 0; k < 25; k++) {
      float d = lerCal();
      if (!isnan(d)) v[n++] = d;
      vTaskDelay(pdMS_TO_TICKS(_cfg.periodoMs));
    }
    if (n >= 10) {                                   // ordena e pega a mediana
      for (uint8_t i = 1; i < n; i++) {
        float x = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
      }
      _base = v[n / 2];
      _baseValida = true;
    } else {
      _baseValida = false;
    }
    _presente = false;
    _contEntra = _contSai = 0;
    _filtro.reset();
    if (tinhaPresenca) enviaEvento(NAN);             // avisa que voltou a "ausente"
  }

  void processa(float d) {
    if (isnan(d)) {                                  // falha: nao muda o estado
      if (++_falhas >= 50) _ok = false;              // ~3 s sem eco
      publicaUltima(NAN);
      return;
    }
    _falhas = 0;
    _ok = true;
    float f = _filtro.add(d);

    if (_baseValida) {
      bool perto = f < _base - _cfg.margemEntrar;
      bool longe = f > _base - _cfg.margemSair;
      if (perto)      { if (_contEntra < 255) _contEntra++; _contSai = 0; }
      else if (longe) { if (_contSai < 255)   _contSai++;   _contEntra = 0; }
      // entre os dois limiares (zona de histerese) os contadores nao mudam

      if (!_presente && _contEntra >= _cfg.leiturasEntrar) { _presente = true;  enviaEvento(f); }
      if (_presente  && _contSai   >= _cfg.leiturasSair)   { _presente = false; enviaEvento(f); }
    }
    publicaUltima(f);
  }

  // ---- A TASK: roda para sempre ----
  void tarefa() {
    calibrarBaseInterno();                           // ambiente deve estar VAZIO no boot
    TickType_t ultimoCiclo = xTaskGetTickCount();
    for (;;) {
      if (_pedirRecalibrar) { _pedirRecalibrar = false; calibrarBaseInterno(); }
      processa(lerCal());
      // Espera ate o PROXIMO multiplo de periodoMs (periodo fixo, sem acumular atraso)
      // e libera a CPU para as outras tasks enquanto espera.
      vTaskDelayUntil(&ultimoCiclo, pdMS_TO_TICKS(_cfg.periodoMs));
    }
  }
};

#endif // PRESENCA_HCSR04_H
