/*
 * temperatura_ds18b20.h
 * Leitura do DS18B20 em uma TASK propria do FreeRTOS.
 *
 * O DS18B20 leva ~750 ms para converter em 12 bits. Na task isso nao trava
 * o resto: ela pede a conversao, dorme, le o resultado e entrega por uma fila
 * de 1 posicao (sempre o valor mais recente).

 */

#ifndef TEMPERATURA_DS18B20_H
#define TEMPERATURA_DS18B20_H

#include <Arduino.h>
#include <math.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

struct LeituraTemp {
  float tempC;     // NAN se invalida
  bool ok;         // false: sensor desconectado, leitura invalida ou falhas seguidas
  uint32_t tMs;
};

class TemperaturaDS18B20 {
 public:
  explicit TemperaturaDS18B20(uint8_t pino, uint32_t periodoMs = 5000)
      : _ow(pino), _ds(&_ow), _periodoMs(periodoMs) {}

  bool begin(UBaseType_t prioridade = 1, BaseType_t core = 1) {
    _ds.begin();
    _ds.setResolution(12);
    _ds.setWaitForConversion(false);       // requestTemperatures() nao bloqueia

    _fila = xQueueCreate(1, sizeof(LeituraTemp));
    if (!_fila) return false;
    return xTaskCreatePinnedToCore(tarefaEstatica, "temp", 3072, this,
                                   prioridade, &_tarefa, core) == pdPASS;
  }

  // Copia a leitura mais recente sem remove-la da fila
  bool ultima(LeituraTemp& l) { return xQueuePeek(_fila, &l, 0) == pdTRUE; }

 private:
  OneWire _ow;
  DallasTemperature _ds;
  uint32_t _periodoMs;
  TaskHandle_t _tarefa = nullptr;
  QueueHandle_t _fila = nullptr;

  static void tarefaEstatica(void* arg) {
    static_cast<TemperaturaDS18B20*>(arg)->tarefa();
  }

  void tarefa() {
    uint8_t falhas = 0;
    float ultimaValida = NAN;
    for (;;) {
      _ds.requestTemperatures();                    // dispara a conversao
      vTaskDelay(pdMS_TO_TICKS(800));               // 12 bits precisam de ~750 ms
      float t = _ds.getTempCByIndex(0);

      // -127 = sensor nao respondeu | 85 = valor de power-on (conversao nao terminou)
      bool valida = (t > -55.0f && t < 125.0f && t != 85.0f && t != DEVICE_DISCONNECTED_C);
      if (valida) { ultimaValida = t; falhas = 0; }
      else if (falhas < 255) falhas++;

      // Tolera 3 falhas seguidas repetindo a ultima leitura boa; depois marca invalida
      LeituraTemp l;
      if (valida)                          l = {t, true, millis()};
      else if (falhas < 3 && !isnan(ultimaValida)) l = {ultimaValida, true, millis()};
      else                                 l = {NAN, false, millis()};
      xQueueOverwrite(_fila, &l);

      uint32_t resto = _periodoMs > 800 ? _periodoMs - 800 : 0;
      vTaskDelay(pdMS_TO_TICKS(resto));
    }
  }
};

#endif // TEMPERATURA_DS18B20_H
