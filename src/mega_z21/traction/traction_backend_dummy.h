/*
 * traction_backend_dummy.h
 * -------------------------
 * DummyTractionBackend: NO toca ningún bus real. Guarda en RAM lo último
 * que la app Z21 mandó y lo devuelve tal cual — es exactamente el
 * comportamiento que tenía mega_z21.ino antes de este refactor (tabla
 * dummyLocos[] + booleans dummyTrackPowerOn/dummyEmergencyStop), solo que
 * ahora vive detrás de ITractionBackend en vez de mezclado con el
 * protocolo Z21.
 *
 * Para qué sirve TODAVÍA teniendo ya un backend XpressNet real (ver
 * traction_backend_xpressnet.h): permite seguir validando el handshake
 * Z21 (AGENT.md, prioridad nº1) y probar la app en el móvil SIN tener el
 * RS485/MultiMaus cableado — cambiar de un backend a otro es una sola
 * línea en mega_z21.ino (ver TRACTION_BACKEND_* al principio del sketch).
 */
#ifndef TRACTION_BACKEND_DUMMY_H
#define TRACTION_BACKEND_DUMMY_H

#include "traction_backend.h"
#include "loco_state_store.h"
#include "accessory_state_store.h"
#include "ext_accessory_state_store.h"

class DummyTractionBackend : public ITractionBackend {
public:
  void begin() override {
    track_ = TrackState();
  }

  void poll() override {
    // Nada que sondear: no hay bus físico detrás.
  }

  bool setTrackPower(bool on) override {
    track_.powerOn = on;
    if (on) track_.emergencyStop = false; // ver PDF 2.6: Power ON también termina el E-Stop
    return true;
  }

  void emergencyStopAll() override {
    track_.emergencyStop = true;
  }

  TrackState getTrackState() const override {
    return track_;
  }

  void setLocoDrive(uint16_t addr, uint8_t stepsCode, uint8_t speedByte) override {
    LocoState *loco = locos_.findOrAlloc(addr);
    loco->stepsCode = stepsCode;
    loco->speedByte = speedByte;
  }

  void setLocoFunction(uint16_t addr, uint8_t index, FunctionOp op) override {
    if (op == static_cast<FunctionOp>(0b11)) return; // "no permitido", ver PDF
    LocoState *loco = locos_.findOrAlloc(addr);
    applySingleFunction(loco, index, op);
  }

  void setLocoFunctionGroup(uint16_t addr, FunctionGroup group, uint8_t value) override {
    LocoState *loco = locos_.findOrAlloc(addr);
    switch (group) {
      case FunctionGroup::F0toF4:
        loco->f0to4 = value & 0x1F; // mismo orden de bits que DB4, ver traction_types.h
        break;
      case FunctionGroup::F5toF8:
        loco->f5to12 = (loco->f5to12 & 0xF0) | (value & 0x0F);
        break;
      case FunctionGroup::F9toF12:
        loco->f5to12 = (loco->f5to12 & 0x0F) | ((value & 0x0F) << 4);
        break;
      case FunctionGroup::F13toF20:
        loco->f13to20 = value;
        break;
      case FunctionGroup::F21toF28:
        loco->f21to28 = value;
        break;
      // Grupos 6-10 (F29-F68, ver comentario en traction_backend.h): a
      // diferencia de los grupos 1-5, aquí el byte del comando ya viene
      // en orden bit0=función más baja tal cual, sin repartir entre dos
      // grupos ni reordenar bits — se guarda directo.
      case FunctionGroup::F29toF36:
        loco->f29to36 = value;
        break;
      case FunctionGroup::F37toF44:
        loco->f37to44 = value;
        break;
      case FunctionGroup::F45toF52:
        loco->f45to52 = value;
        break;
      case FunctionGroup::F53toF60:
        loco->f53to60 = value;
        break;
      case FunctionGroup::F61toF68:
        loco->f61to68 = value;
        break;
    }
  }

  void requestLocoRefresh(uint16_t addr) override {
    (void)addr; // no-op: el estado dummy ya está siempre al día
  }

  const LocoState *getLocoState(uint16_t addr) override {
    return locos_.findOrAlloc(addr);
  }

  void purgeLoco(uint16_t addr) override {
    locos_.release(addr);
  }

  void setTurnout(uint16_t addr, bool output, bool activate) override {
    AccessoryState *acc = accessories_.findOrAlloc(addr);
    // Sin bus físico detrás: se simula que la salida activada "prende" al
    // momento (A=1) y el estado se queda tal cual al soltar el pulso
    // (A=0) — igual que haría un decodificador de accesorios biestable
    // real, que no vuelve a NotSwitched solo porque se corte la
    // alimentación de la bobina.
    if (activate) {
      acc->position = output ? AccessoryPosition::Output2 : AccessoryPosition::Output1;
    }
  }

  void requestTurnoutRefresh(uint16_t addr) override {
    (void)addr; // no-op: el estado dummy ya está siempre al día
  }

  const AccessoryState *getTurnoutState(uint16_t addr) override {
    return accessories_.findOrAlloc(addr);
  }

  void setExtAccessory(uint16_t rawAddr, uint8_t state) override {
    // Sin bus físico detrás: el valor mandado por la app pasa a ser el
    // "último conocido" al instante, igual que hace setTurnout() con la
    // posición. hasData=true a partir de aquí: ya sabemos con certeza
    // qué es lo último que se le mandó a esta dirección.
    ExtAccessoryState *ext = extAccessories_.findOrAlloc(rawAddr);
    ext->state = state;
    ext->hasData = true;
  }

  void requestExtAccessoryRefresh(uint16_t rawAddr) override {
    (void)rawAddr; // no-op: el estado dummy ya está siempre al día
  }

  const ExtAccessoryState *getExtAccessoryState(uint16_t rawAddr) override {
    return extAccessories_.findOrAlloc(rawAddr);
  }

private:
  // Aplica un cambio de una única función (LAN_X_SET_LOCO_FUNCTION). F0-F4
  // usan el orden de bits especial de DB4 (ver traction_types.h); F5-F68
  // son bit directo dentro de su byte (bit0 = función más baja del
  // grupo). TTNNNNNN solo tiene 6 bits de índice (PDF 4.3.1), así que en
  // la práctica esta función nunca recibe index>63 por esa vía — F64-F68
  // solo llegan por setLocoFunctionGroup(F61toF68), no por aquí.
  static void applySingleFunction(LocoState *loco, uint8_t index, FunctionOp op) {
    uint8_t *targetByte;
    uint8_t bitPos;
    if (index <= 4) {
      targetByte = &loco->f0to4;
      switch (index) {
        case 0: bitPos = 4; break; // L
        case 1: bitPos = 0; break; // J
        case 2: bitPos = 1; break; // H
        case 3: bitPos = 2; break; // G
        default: bitPos = 3; break; // F (index==4)
      }
    } else if (index <= 12) {
      targetByte = &loco->f5to12; bitPos = index - 5;
    } else if (index <= 20) {
      targetByte = &loco->f13to20; bitPos = index - 13;
    } else if (index <= 28) {
      targetByte = &loco->f21to28; bitPos = index - 21;
    } else if (index <= 36) {
      targetByte = &loco->f29to36; bitPos = index - 29;
    } else if (index <= 44) {
      targetByte = &loco->f37to44; bitPos = index - 37;
    } else if (index <= 52) {
      targetByte = &loco->f45to52; bitPos = index - 45;
    } else if (index <= 60) {
      targetByte = &loco->f53to60; bitPos = index - 53;
    } else if (index <= 68) {
      targetByte = &loco->f61to68; bitPos = index - 61;
    } else {
      return; // fuera de rango (no debería llegar aquí, ver comentario arriba)
    }
    uint8_t mask = (uint8_t)(1 << bitPos);
    if (op == FunctionOp::Off) *targetByte &= ~mask;
    else if (op == FunctionOp::On) *targetByte |= mask;
    else *targetByte ^= mask; // Toggle
  }

  TrackState track_;
  LocoStateStore locos_;
  AccessoryStateStore accessories_;
  ExtAccessoryStateStore extAccessories_;
};

#endif // TRACTION_BACKEND_DUMMY_H
