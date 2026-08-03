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
    }
  }

  void requestLocoRefresh(uint16_t addr) override {
    (void)addr; // no-op: el estado dummy ya está siempre al día
  }

  const LocoState *getLocoState(uint16_t addr) override {
    return locos_.findOrAlloc(addr);
  }

private:
  // Aplica un cambio de una única función (LAN_X_SET_LOCO_FUNCTION). F0-F4
  // usan el orden de bits especial de DB4 (ver traction_types.h); F5-F28
  // son bit directo dentro de su byte. F29+ quedan fuera de esta tabla
  // (no hay campo reservado para ellas todavía en LocoState).
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
    } else {
      return; // F29+: fuera de alcance todavía
    }
    uint8_t mask = (uint8_t)(1 << bitPos);
    if (op == FunctionOp::Off) *targetByte &= ~mask;
    else if (op == FunctionOp::On) *targetByte |= mask;
    else *targetByte ^= mask; // Toggle
  }

  TrackState track_;
  LocoStateStore locos_;
};

#endif // TRACTION_BACKEND_DUMMY_H
