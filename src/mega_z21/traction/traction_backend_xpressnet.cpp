/*
 * traction_backend_xpressnet.cpp
 * --------------------------------
 * Ver traction_backend_xpressnet.h para el contexto completo, las
 * dependencias externas y las asunciones pendientes de validar contra
 * hardware real.
 */
#include "traction_backend_xpressnet.h"
#include "traction_config.h"

// Ver el motivo de esta guarda en traction_config.h y en la cabecera de
// traction_backend_xpressnet.h: si el backend seleccionado NO es este,
// este .cpp entero debe compilar a "nada" — en particular, sin necesitar
// la librería externa <XpressNet.h> instalada.
#if TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_XPRESSNET

#include <XpressNet.h> // librería externa Digital-MoBa/XpressNet, instalar aparte (ver .h)

XpressNetTractionBackend *g_activeXpressNetBackend = nullptr;

// ---------------------------------------------------------------------
// Conversión de formatos (ver punto 1 y 2 de "ASUNCIONES A VALIDAR" en
// el .h) — aisladas en funciones propias para que, si al probar con la
// MultiMaus real algo sale invertido, el sitio a mirar sea obvio y no
// haya que rebuscar por todo el fichero.
// ---------------------------------------------------------------------

// stepsCode Z21 (0=14, 2=28, 4=128) -> código Steps de XpressNetClass,
// documentado en XpressNet.h como el campo 'mode' de xLokSts (0=14,
// 1=27, 2=28, 3=128). El valor "1=27" es una variante DCC antigua que el
// protocolo Z21 no expone nunca, así que no tiene equivalente de entrada
// aquí (solo se recibe como salida en notifyLokAll, ver z21StepsFromXpressnet).
static uint8_t xpressnetStepsFromZ21(uint8_t stepsCode) {
  switch (stepsCode) {
    case 0: return 0; // 14 pasos
    case 2: return 2; // 28 pasos
    default: return 3; // 128 pasos (incluye el 0x13 y variantes que ya
                        // trata como "128" el resto de mega_z21.ino)
  }
}

// Inverso: código Steps de XpressNetClass (recibido en notifyLokAll) ->
// stepsCode Z21 para rellenar LocoState. steps==1 (27 pasos) no tiene
// equivalente Z21 real — se reporta como 28 pasos (lo más parecido) en
// vez de inventar un valor fuera de tabla; si esto se llega a ver en la
// práctica con la MultiMaus, es un caso a revisar aquí primero.
static uint8_t z21StepsFromXpressnet(uint8_t xnSteps) {
  switch (xnSteps) {
    case 0: return 0; // 14 pasos
    case 3: return 4; // 128 pasos
    default: return 2; // 28 pasos (incluye el caso 27 pasos sin equivalente)
  }
}

XpressNetTractionBackend::XpressNetTractionBackend() {}

void XpressNetTractionBackend::begin() {
  g_activeXpressNetBackend = this;
  track_ = TrackState();
  // start(XAdr, XControl): en Mega/AVR (no ESP) la librería fuerza
  // Serial1 internamente (ver .h) — aquí solo se pasa la dirección
  // propia y el pin de control DE/RE del MAX485.
  XpressNet.start(TRACTION_XPRESSNET_MY_ADDRESS, TRACTION_XPRESSNET_DE_RE_PIN);
}

void XpressNetTractionBackend::poll() {
  // receive() es la única función de la librería que hay que llamar en
  // cada vuelta de loop(): revisa si hay un paquete XpressNet completo y,
  // si lo hay, dispara desde dentro los callbacks notifyXXX (weak) de
  // más abajo. No bloquea si no hay nada pendiente.
  XpressNet.receive();
}

bool XpressNetTractionBackend::setTrackPower(bool on) {
  // setPower() de la librería espera csNormal/csTrackVoltageOff (ver
  // XpressNet.h, constantes csXxx) como valor a REPORTAR al bus, no un
  // simple booleano de encendido — csNormal = "todo va bien / vía
  // activa", csTrackVoltageOff = vía cortada.
  bool ok = XpressNet.setPower(on ? csNormal : csTrackVoltageOff);
  if (ok) {
    track_.powerOn = on;
    if (on) track_.emergencyStop = false; // ver PDF Z21 2.6
  }
  return ok;
}

void XpressNetTractionBackend::emergencyStopAll() {
  XpressNet.setHalt();
  track_.emergencyStop = true;
}

TrackState XpressNetTractionBackend::getTrackState() const {
  return track_;
}

void XpressNetTractionBackend::setLocoDrive(uint16_t addr, uint8_t stepsCode, uint8_t speedByte) {
  uint8_t adrHigh = (uint8_t)((addr >> 8) & 0x3F);
  uint8_t adrLow = (uint8_t)(addr & 0xFF);
  XpressNet.setLocoDrive(adrHigh, adrLow, xpressnetStepsFromZ21(stepsCode), speedByte);
  // No se actualiza locos_ aquí a ciegas: el estado "confirmado" llega
  // más tarde por notifyLokAll/onLokAll cuando el bus responde de
  // verdad. Mientras tanto, getLocoState() sigue devolviendo lo último
  // conocido (posiblemente el valor anterior) — ver comentario de
  // pendingBusConfirmation en traction_types.h.
  LocoState *loco = locos_.findOrAlloc(addr);
  loco->pendingBusConfirmation = true;
}

void XpressNetTractionBackend::setLocoFunction(uint16_t addr, uint8_t index, FunctionOp op) {
  uint8_t adrHigh = (uint8_t)((addr >> 8) & 0x3F);
  uint8_t adrLow = (uint8_t)(addr & 0xFF);
  XpressNet.setLocoFunc(adrHigh, adrLow, static_cast<uint8_t>(op), index);
}

void XpressNetTractionBackend::setLocoFunctionGroup(uint16_t addr, FunctionGroup group, uint8_t value) {
  switch (group) {
    case FunctionGroup::F0toF4:
      XpressNet.setFunc0to4(addr, value);
      break;
    case FunctionGroup::F5toF8:
      XpressNet.setFunc5to8(addr, value);
      break;
    case FunctionGroup::F9toF12:
      XpressNet.setFunc9to12(addr, value);
      break;
    case FunctionGroup::F13toF20:
      XpressNet.setFunc13to20(addr, value);
      break;
    case FunctionGroup::F21toF28:
      XpressNet.setFunc21to28(addr, value);
      break;
  }
}

void XpressNetTractionBackend::requestLocoRefresh(uint16_t addr) {
  uint8_t adrHigh = (uint8_t)((addr >> 8) & 0x3F);
  uint8_t adrLow = (uint8_t)(addr & 0xFF);
  XpressNet.getLocoInfo(adrHigh, adrLow);   // F0-F12
  XpressNet.getLocoFunc(adrHigh, adrLow);   // F13-F28
}

const LocoState *XpressNetTractionBackend::getLocoState(uint16_t addr) {
  return locos_.findOrAlloc(addr);
}

void XpressNetTractionBackend::setTurnout(uint16_t addr, bool output, bool activate) {
  uint8_t adrHigh = (uint8_t)((addr >> 8) & 0xFF); // sin enmascarar: dirección
  uint8_t adrLow = (uint8_t)(addr & 0xFF);          // de accesorio, no de loco
  // Formato de 'Pos' — ver punto 4 de "ASUNCIONES A VALIDAR" en el .h.
  uint8_t pos = (uint8_t)((activate ? 0x08 : 0x00) | (output ? 0x01 : 0x00));
  XpressNet.setTrntPos(adrHigh, adrLow, pos);
  // No se actualiza accessories_ aquí a ciegas, igual que setLocoDrive no
  // actualiza locos_: el estado confirmado llega por notifyTrnt/onTrnt
  // cuando el bus responde de verdad. getTurnoutState() mientras tanto
  // sigue devolviendo el último valor conocido.
}

void XpressNetTractionBackend::requestTurnoutRefresh(uint16_t addr) {
  uint8_t adrHigh = (uint8_t)((addr >> 8) & 0xFF);
  uint8_t adrLow = (uint8_t)(addr & 0xFF);
  XpressNet.getTrntInfo(adrHigh, adrLow);
}

const AccessoryState *XpressNetTractionBackend::getTurnoutState(uint16_t addr) {
  return accessories_.findOrAlloc(addr);
}

void XpressNetTractionBackend::onXNetStatus(uint8_t ledState) {
  (void)ledState; // TODO: llevar esto al log de pantalla cuando se conecte
                  // el RS485 real (displayLogf vive en mega_z21.ino, no aquí
                  // para no acoplar este backend a la pantalla).
}

void XpressNetTractionBackend::onXNetPower(uint8_t state) {
  // state llega en formato csXxx (ver XpressNet.h): csTrackVoltageOff
  // tiene el bit correspondiente puesto si la vía está cortada.
  track_.powerOn = !(state & csTrackVoltageOff);
  track_.emergencyStop = (state & csEmergencyStop) != 0;
  track_.shortCircuit = (state & csShortCircuit) != 0;
  track_.serviceModeActive = (state & csServiceMode) != 0;
}

void XpressNetTractionBackend::onLokAll(uint8_t adrHigh, uint8_t adrLow, bool busy,
                                         uint8_t steps, uint8_t speed, uint8_t direction,
                                         uint8_t f0, uint8_t f1, uint8_t f2, uint8_t f3,
                                         bool req) {
  (void)busy; (void)req; // sin uso todavía: sin gestión de slots ocupados por
                         // otros clientes en esta primera versión del backend
  uint16_t addr = ((uint16_t)(adrHigh & 0x3F) << 8) | adrLow;
  LocoState *loco = locos_.findOrAlloc(addr);
  loco->stepsCode = z21StepsFromXpressnet(steps);
  // speedByte Z21 = bit7 sentido + bits0-6 velocidad (ver traction_types.h)
  loco->speedByte = (uint8_t)((direction ? 0x80 : 0x00) | (speed & 0x7F));
  loco->f0to4 = f0 & 0x1F;   // mismo orden de bits que DB4, ver cabecera del .h
  loco->f5to12 = f1;         // F1: F12..F5, idéntico a DB5 (ver traction_types.h)
  loco->f13to20 = f2;        // F2: F20..F13, idéntico a DB6
  loco->f21to28 = f3;        // F3: F28..F21, idéntico a DB7
  loco->pendingBusConfirmation = false;
}

void XpressNetTractionBackend::onTrnt(uint8_t adrHigh, uint8_t adrLow, uint8_t pos) {
  uint16_t addr = ((uint16_t)adrHigh << 8) | adrLow; // sin enmascarar, ver setTurnout()
  AccessoryState *acc = accessories_.findOrAlloc(addr);
  acc->position = static_cast<AccessoryPosition>(pos & 0x03);
}

// ---------------------------------------------------------------------
// Callbacks "weak" de la librería XpressNetClass (espacio de nombres
// global, ver XpressNet.h) — solo pueden existir UNA vez en todo el
// firmware (por eso viven en este único .cpp) y reenvían al backend
// activo. Si algún día se necesitara más de un backend XpressNet a la
// vez, este es el único sitio a rediseñar (no debería hacer falta: la
// propia librería tampoco lo soporta, ver active_object en XpressNet.h).
// ---------------------------------------------------------------------
void notifyXNetStatus(uint8_t LedState) {
  if (g_activeXpressNetBackend) g_activeXpressNetBackend->onXNetStatus(LedState);
}

void notifyXNetPower(uint8_t State) {
  if (g_activeXpressNetBackend) g_activeXpressNetBackend->onXNetPower(State);
}

void notifyLokAll(uint8_t Adr_High, uint8_t Adr_Low, boolean Busy, uint8_t Steps,
                  uint8_t Speed, uint8_t Direction, uint8_t F0, uint8_t F1,
                  uint8_t F2, uint8_t F3, boolean Req) {
  if (g_activeXpressNetBackend) {
    g_activeXpressNetBackend->onLokAll(Adr_High, Adr_Low, Busy, Steps, Speed,
                                        Direction, F0, F1, F2, F3, Req);
  }
}

void notifyTrnt(uint8_t Adr_High, uint8_t Adr_Low, uint8_t Pos) {
  if (g_activeXpressNetBackend) {
    g_activeXpressNetBackend->onTrnt(Adr_High, Adr_Low, Pos);
  }
}

#endif // TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_XPRESSNET
