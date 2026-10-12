/*
 * saidas.h
 * Saida digital simples (LED, rele) com logica ativa-alta ou ativa-baixa.
 *
 * Modulos de rele baratos costumam ser ATIVOS EM BAIXO: IN = LOW liga o rele.
 * Com ativoBaixo = true, voce usa set(true) = ligado e a classe inverte o pino.
 */

#ifndef SAIDAS_H
#define SAIDAS_H

#include <Arduino.h>
#include <strings.h>

class Saida {
 public:
  Saida(uint8_t pino, bool ativoBaixo = false) : _pino(pino), _ativoBaixo(ativoBaixo) {}

  void begin() {                 // sempre inicia DESLIGADA
    _estado = false;
    pinMode(_pino, OUTPUT);
    aplica();
  }
  void set(bool ligado) { _estado = ligado; aplica(); }
  void toggle() { set(!_estado); }
  bool estado() const { return _estado; }

  // Interpreta texto vindo do MQTT: on/off/1/0/true/false/toggle (sem diferenciar maiusculas).
  // Retorna false se o texto nao for um comando valido (nada muda).
  bool comando(const char* msg) {
    if (!strcasecmp(msg, "on") || !strcmp(msg, "1") || !strcasecmp(msg, "true"))        set(true);
    else if (!strcasecmp(msg, "off") || !strcmp(msg, "0") || !strcasecmp(msg, "false")) set(false);
    else if (!strcasecmp(msg, "toggle"))                                                 toggle();
    else return false;
    return true;
  }

 private:
  uint8_t _pino;
  bool _ativoBaixo;
  bool _estado = false;

  void aplica() { digitalWrite(_pino, (_estado != _ativoBaixo) ? HIGH : LOW); }
};

#endif // SAIDAS_H
