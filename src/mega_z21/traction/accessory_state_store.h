/*
 * accessory_state_store.h
 * ------------------------
 * AccessoryStateStore: la tabla en RAM de "última AccessoryState conocida
 * por dirección" — mismo diseño y mismo motivo que LocoStateStore (ver
 * loco_state_store.h): responsabilidad única, sin saber nada de Z21 ni de
 * XpressNet, reutilizable por composición desde cualquier backend
 * (DummyTractionBackend, XpressNetTractionBackend, y el futuro backend
 * LocoNet/DCC directo).
 *
 * ÚNICA DIFERENCIA respecto a LocoStateStore: el marcador de "slot libre"
 * es 0xFFFF, no 0 — una dirección de accesorio 0 SÍ es válida en el
 * protocolo Z21 (PDF sección 5.1, sin el enmascarado "& 0x3F" que sí
 * lleva la dirección de loco), así que no se puede reutilizar 0 como
 * centinela igual que hace LocoState::address. Ver traction_types.h.
 *
 * Política de slot lleno: igual que LocoStateStore, sin LRU real — si no
 * hay slot libre, recicla el slot 0. Suficiente para MAX_TRACKED_
 * ACCESSORIES accesorios activos a la vez.
 */
#ifndef ACCESSORY_STATE_STORE_H
#define ACCESSORY_STATE_STORE_H

#include "traction_types.h"

class AccessoryStateStore {
public:
  AccessoryStateStore() {
    for (uint8_t i = 0; i < MAX_TRACKED_ACCESSORIES; i++) {
      slots_[i] = AccessoryState();
    }
  }

  // Busca el accesorio por dirección; si no existe, ocupa un slot libre
  // con los valores por defecto de AccessoryState (posición
  // NotSwitched). Si la tabla está llena, recicla el slot 0.
  AccessoryState *findOrAlloc(uint16_t addr) {
    for (uint8_t i = 0; i < MAX_TRACKED_ACCESSORIES; i++) {
      if (slots_[i].address == addr) return &slots_[i];
    }
    for (uint8_t i = 0; i < MAX_TRACKED_ACCESSORIES; i++) {
      if (slots_[i].address == 0xFFFF) {
        slots_[i] = AccessoryState();
        slots_[i].address = addr;
        return &slots_[i];
      }
    }
    slots_[0] = AccessoryState();
    slots_[0].address = addr;
    return &slots_[0];
  }

  // Solo lectura: null si la dirección no está en la tabla todavía (no
  // reserva slot).
  const AccessoryState *find(uint16_t addr) const {
    for (uint8_t i = 0; i < MAX_TRACKED_ACCESSORIES; i++) {
      if (slots_[i].address == addr) return &slots_[i];
    }
    return nullptr;
  }

private:
  AccessoryState slots_[MAX_TRACKED_ACCESSORIES];
};

#endif // ACCESSORY_STATE_STORE_H
