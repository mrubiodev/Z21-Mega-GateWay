/*
 * loco_state_store.h
 * -------------------
 * LocoStateStore: la tabla en RAM de "última LocoState conocida por
 * dirección". Antes esta lógica (findOrAllocLoco) vivía suelta dentro de
 * mega_z21.ino operando directamente sobre un array global de
 * DummyLocoState. Se separa aquí por responsabilidad única (SRP): esto
 * es SOLO una caché indexada por dirección, no sabe nada de Z21 ni de
 * XpressNet ni de ningún bus. La reutilizan por composición:
 *   - DummyTractionBackend (estado "de mentira", lo único que hay)
 *   - XpressNetTractionBackend (caché de lo último que confirmó el bus
 *     real vía notifyLokAll)
 * y la reutilizaría igual un futuro backend LocoNet/DCC directo sin
 * duplicar esta tabla ni su política de reciclado de slots.
 *
 * Política de slot lleno: igual que antes, sin LRU real — si no hay
 * slot libre, recicla el slot 0. Suficiente para MAX_TRACKED_LOCOS pocas
 * locos activas a la vez (ver traction_types.h). Si en el futuro hace
 * falta LRU de verdad, este es el único sitio a tocar.
 */
#ifndef LOCO_STATE_STORE_H
#define LOCO_STATE_STORE_H

#include "traction_types.h"

class LocoStateStore {
public:
  LocoStateStore() {
    for (uint8_t i = 0; i < MAX_TRACKED_LOCOS; i++) {
      slots_[i] = LocoState();
    }
  }

  // Busca la locomotora por dirección; si no existe, ocupa un slot libre
  // con los valores por defecto de LocoState (128 pasos, parada, sentido
  // adelante, funciones a 0). Si la tabla está llena, recicla el slot 0.
  LocoState *findOrAlloc(uint16_t addr) {
    for (uint8_t i = 0; i < MAX_TRACKED_LOCOS; i++) {
      if (slots_[i].address == addr) return &slots_[i];
    }
    for (uint8_t i = 0; i < MAX_TRACKED_LOCOS; i++) {
      if (slots_[i].address == 0) {
        slots_[i] = LocoState();
        slots_[i].address = addr;
        return &slots_[i];
      }
    }
    slots_[0] = LocoState();
    slots_[0].address = addr;
    return &slots_[0];
  }

  // Solo lectura: null si la dirección no está en la tabla todavía (no
  // reserva slot). Útil para "¿tengo ya algo que responder?" sin forzar
  // una entrada nueva con valores por defecto.
  const LocoState *find(uint16_t addr) const {
    for (uint8_t i = 0; i < MAX_TRACKED_LOCOS; i++) {
      if (slots_[i].address == addr) return &slots_[i];
    }
    return nullptr;
  }

  // Libera el slot de una dirección si existe (LAN_X_PURGE_LOCO, PDF
  // 4.6): lo devuelve a los valores por defecto de LocoState, incluyendo
  // address=0 (slot libre de nuevo) — así la próxima consulta a esa
  // dirección arranca de cero en vez de arrastrar el último estado
  // conocido antes de la purga. No-op si la dirección no estaba en la
  // tabla (findOrAlloc() la creará igualmente la próxima vez que haga
  // falta, ver PDF: "sending will start again as soon as a new drive or
  // function command is sent to the same locomotive address").
  void release(uint16_t addr) {
    for (uint8_t i = 0; i < MAX_TRACKED_LOCOS; i++) {
      if (slots_[i].address == addr) {
        slots_[i] = LocoState();
        return;
      }
    }
  }

private:
  LocoState slots_[MAX_TRACKED_LOCOS];
};

#endif // LOCO_STATE_STORE_H
