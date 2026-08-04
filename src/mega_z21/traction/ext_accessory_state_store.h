/*
 * ext_accessory_state_store.h
 * -----------------------------
 * ExtAccessoryStateStore: la tabla en RAM de "último ExtAccessoryState
 * conocido por RawAddress" — mismo diseño y mismo motivo que
 * AccessoryStateStore (ver accessory_state_store.h): responsabilidad
 * única, sin saber nada de Z21 ni de XpressNet, reutilizable por
 * composición desde cualquier backend.
 *
 * POR QUÉ NO ES LA MISMA TABLA QUE AccessoryStateStore: aunque el diseño
 * de la clase es idéntico letra por letra, las direcciones que gestiona
 * cada una viven en espacios de direcciones DISTINTOS a nivel de
 * protocolo Z21 (RawAddress de RCN-213 aquí, FAdr con conversión a
 * puerto/salida en AccessoryState — ver traction_types.h). Compartir la
 * tabla mezclaría por accidente un accesorio normal #4 con un accesorio
 * extendido RawAddress=4, que no tienen ninguna relación entre sí.
 *
 * Política de slot lleno: igual que AccessoryStateStore, sin LRU real —
 * si no hay slot libre, recicla el slot 0. Suficiente para MAX_TRACKED_
 * EXT_ACCESSORIES accesorios extendidos activos a la vez (señales de más
 * de 2 aspectos son minoría frente a agujas normales en una maqueta
 * típica, de ahí que la tabla sea más pequeña que MAX_TRACKED_ACCESSORIES).
 */
#ifndef EXT_ACCESSORY_STATE_STORE_H
#define EXT_ACCESSORY_STATE_STORE_H

#include "traction_types.h"

class ExtAccessoryStateStore {
public:
  ExtAccessoryStateStore() {
    for (uint8_t i = 0; i < MAX_TRACKED_EXT_ACCESSORIES; i++) {
      slots_[i] = ExtAccessoryState();
    }
  }

  // Busca el accesorio extendido por RawAddress; si no existe, ocupa un
  // slot libre con los valores por defecto de ExtAccessoryState (state=0,
  // hasData=false — "todavía no se ha mandado ni conocido nada real para
  // esta dirección"). Si la tabla está llena, recicla el slot 0.
  ExtAccessoryState *findOrAlloc(uint16_t rawAddr) {
    for (uint8_t i = 0; i < MAX_TRACKED_EXT_ACCESSORIES; i++) {
      if (slots_[i].address == rawAddr) return &slots_[i];
    }
    for (uint8_t i = 0; i < MAX_TRACKED_EXT_ACCESSORIES; i++) {
      if (slots_[i].address == 0xFFFF) {
        slots_[i] = ExtAccessoryState();
        slots_[i].address = rawAddr;
        return &slots_[i];
      }
    }
    slots_[0] = ExtAccessoryState();
    slots_[0].address = rawAddr;
    return &slots_[0];
  }

  // Solo lectura: null si la dirección no está en la tabla todavía (no
  // reserva slot).
  const ExtAccessoryState *find(uint16_t rawAddr) const {
    for (uint8_t i = 0; i < MAX_TRACKED_EXT_ACCESSORIES; i++) {
      if (slots_[i].address == rawAddr) return &slots_[i];
    }
    return nullptr;
  }

private:
  ExtAccessoryState slots_[MAX_TRACKED_EXT_ACCESSORIES];
};

#endif // EXT_ACCESSORY_STATE_STORE_H
