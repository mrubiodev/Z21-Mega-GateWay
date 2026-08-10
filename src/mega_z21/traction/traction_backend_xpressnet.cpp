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

// Cada cuánto se reintenta XpressNet.getresultCV() mientras hay una CV
// pendiente, y cuánto se espera antes de rendirse y sintetizar un
// LAN_X_CV_NACK (ver punto 7 de "ASUNCIONES A VALIDAR" en el .h — no hay
// forma de confirmar estos tiempos sin hardware real, son un punto de
// partida razonable, no un valor sagrado).
#define CV_POLL_INTERVAL_MS 100UL
#define CV_TIMEOUT_MS 5000UL

void XpressNetTractionBackend::poll() {
  // receive() es la única función de la librería que hay que llamar en
  // cada vuelta de loop(): revisa si hay un paquete XpressNet completo y,
  // si lo hay, dispara desde dentro los callbacks notifyXXX (weak) de
  // más abajo. No bloquea si no hay nada pendiente.
  XpressNet.receive();

  if (cvPending_) {
    unsigned long now = millis();
    if (now - lastCvPollMs_ >= CV_POLL_INTERVAL_MS) {
      lastCvPollMs_ = now;
      XpressNet.getresultCV(); // puede disparar notifyCVResult/notifyCVInfo desde dentro
    }
    if (cvPending_ && (now - cvRequestStartMs_ >= CV_TIMEOUT_MS)) {
      cvPending_ = false;
      if (changeCb_) changeCb_({TractionEventType::CvNack, 0, 0});
    }
  }
}

bool XpressNetTractionBackend::cvRead(uint16_t cvAddress) {
  if (cvAddress > 255) return false; // ver punto 7a de "ASUNCIONES A VALIDAR" en el .h
  if (cvPending_) return false; // ya hay otra CV en curso, ver punto 7 del .h — no hay cola
  // cvPending_ se fija ANTES de llamar a la librería, no después: ver el
  // comentario largo sobre writeCVMode() más abajo en cvWrite() — el
  // mismo riesgo de orden aplica aquí por simetría/seguridad, aunque
  // readCVMode() (a diferencia de writeCVMode()) no se ha confirmado que
  // dispare notifyCVResult/notifyCVInfo de forma síncrona.
  cvPending_ = true;
  cvRequestStartMs_ = millis();
  lastCvPollMs_ = cvRequestStartMs_;
  XpressNet.readCVMode((byte)cvAddress);
  return true;
}

bool XpressNetTractionBackend::cvWrite(uint16_t cvAddress, uint8_t value) {
  if (cvAddress > 255) return false; // ver punto 7a de "ASUNCIONES A VALIDAR" en el .h
  if (cvPending_) return false; // ya hay otra CV en curso, ver punto 7 del .h — no hay cola
  // ORDEN CRÍTICO — confirmado leyendo el código fuente real de
  // Digital-MoBa/XpressNet (no solo su cabecera): writeCVMode() llama a
  // notifyCVResult() de forma SÍNCRONA, dentro de la misma llamada, SIN
  // esperar ninguna confirmación real del bus (ni siquiera llama a
  // getresultCV() — esa línea está comentada en el código fuente de la
  // librería). Eso significa que onCVResult() (más abajo) puede ejecutarse
  // y poner cvPending_=false ANTES de que writeCVMode() siquiera retorne.
  // Si cvPending_=true se fijara DESPUÉS de la llamada (como en una
  // versión anterior de este código), se sobreescribiría ese false recién
  // puesto, dejando cvPending_ atascado en true PARA SIEMPRE — bloqueando
  // toda escritura de CV futura hasta reiniciar el Mega. Ver punto 7b de
  // "ASUNCIONES A VALIDAR" en el .h, ya actualizado con este hallazgo.
  cvPending_ = true;
  cvRequestStartMs_ = millis();
  lastCvPollMs_ = cvRequestStartMs_;
  XpressNet.writeCVMode((byte)cvAddress, value);
  return true;
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

// Aplica F29-F68 (index 29-68) SOLO al LocoState en RAM — ver punto 6 de
// "ASUNCIONES A VALIDAR" en el .h. index 0-28 nunca llega aquí (se
// filtra antes en setLocoFunction()); esta función asume ese rango.
// Mismo orden de bits que applySingleFunction() del backend dummy
// (bit0 = función más baja del grupo correspondiente).
static void applySingleFunctionLocalOnly(LocoState *loco, uint8_t index, FunctionOp op) {
  uint8_t *targetByte;
  uint8_t bitPos;
  if (index <= 36) {
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
    return; // fuera de rango — no debería llegar aquí (TTNNNNNN son 6 bits, máx 63)
  }
  uint8_t mask = (uint8_t)(1 << bitPos);
  if (op == FunctionOp::Off) *targetByte &= ~mask;
  else if (op == FunctionOp::On) *targetByte |= mask;
  else *targetByte ^= mask; // Toggle
}

void XpressNetTractionBackend::setLocoFunction(uint16_t addr, uint8_t index, FunctionOp op) {
  uint8_t adrHigh = (uint8_t)((addr >> 8) & 0x3F);
  uint8_t adrLow = (uint8_t)(addr & 0xFF);
  if (index <= 28) {
    XpressNet.setLocoFunc(adrHigh, adrLow, static_cast<uint8_t>(op), index);
    return;
  }
  // F29-F63 (ver punto 6 de "ASUNCIONES A VALIDAR" en el .h): no hay
  // forma de mandar esto por esta librería/X-Bus, así que solo se
  // actualiza el LocoState en RAM — mismo patrón que setExtAccessory().
  if (op == static_cast<FunctionOp>(0b11)) return; // "no permitido", igual que el dummy
  LocoState *loco = locos_.findOrAlloc(addr);
  applySingleFunctionLocalOnly(loco, index, op);
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
    // F29-F68 (grupos 6-10): ver punto 6 de "ASUNCIONES A VALIDAR" en el
    // .h — la librería no tiene ningún setFuncNtoM() para esto, así que
    // solo se actualiza el LocoState en RAM, nada sale hacia la vía.
    case FunctionGroup::F29toF36: {
      LocoState *loco = locos_.findOrAlloc(addr);
      loco->f29to36 = value;
      break;
    }
    case FunctionGroup::F37toF44: {
      LocoState *loco = locos_.findOrAlloc(addr);
      loco->f37to44 = value;
      break;
    }
    case FunctionGroup::F45toF52: {
      LocoState *loco = locos_.findOrAlloc(addr);
      loco->f45to52 = value;
      break;
    }
    case FunctionGroup::F53toF60: {
      LocoState *loco = locos_.findOrAlloc(addr);
      loco->f53to60 = value;
      break;
    }
    case FunctionGroup::F61toF68: {
      LocoState *loco = locos_.findOrAlloc(addr);
      loco->f61to68 = value;
      break;
    }
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

// LAN_X_PURGE_LOCO (PDF 4.6). A diferencia de la limitación de
// setExtAccessory() más arriba, aquí NO hace falta ninguna nota especial:
// Digital-MoBa/XpressNet no implementa un bucle de refresco periódico de
// comandos de tracción por su cuenta (cada setLocoDrive()/setLocoFunction*
// de este backend es un envío puntual disparado por la app Z21, no algo
// que se repita solo), así que "purgar" no tiene nada que cancelar en el
// bus — el único efecto real y honesto que podemos dar es olvidar la
// caché local, igual que en DummyTractionBackend.
void XpressNetTractionBackend::purgeLoco(uint16_t addr) {
  locos_.release(addr);
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

// Ver punto 5 de "ASUNCIONES A VALIDAR" en el .h: Digital-MoBa/XpressNet
// no expone ningún método para el paquete DCCext (extended accessory
// decoder, RCN-213) que necesita LAN_X_SET_EXT_ACCESSORY — solo tiene
// setTrntPos() para el paquete "basic accessory decoder" de 2 salidas
// (el mismo que ya usa setTurnout() más arriba). Por eso esta v1 SOLO
// actualiza el estado en RAM: la app Z21 obtiene una respuesta correcta
// y consistente (LAN_X_EXT_ACCESSORY_INFO con el último valor mandado),
// pero no se envía nada real hacia la MultiMaus/vía. Esto es DISTINTO de
// pendingBusConfirmation (locos) o de "esperar la respuesta async del
// bus" (turnouts normales, ver onTrnt): aquí no hay bus que consultar en
// absoluto con esta librería, así que hasData se marca true de inmediato
// — es el último comando aceptado por esta central, no una confirmación
// de que un decodificador físico lo haya recibido.
void XpressNetTractionBackend::setExtAccessory(uint16_t rawAddr, uint8_t state) {
  ExtAccessoryState *ext = extAccessories_.findOrAlloc(rawAddr);
  ext->state = state;
  ext->hasData = true;
}

void XpressNetTractionBackend::requestExtAccessoryRefresh(uint16_t rawAddr) {
  (void)rawAddr; // no-op: no hay forma de consultar esto en el bus real
                 // con esta librería (ver comentario de setExtAccessory).
}

const ExtAccessoryState *XpressNetTractionBackend::getExtAccessoryState(uint16_t rawAddr) {
  return extAccessories_.findOrAlloc(rawAddr);
}

void XpressNetTractionBackend::onXNetStatus(uint8_t ledState) {
  (void)ledState; // TODO: llevar esto al log de pantalla cuando se conecte
                  // el RS485 real (displayLogf vive en mega_z21.ino, no aquí
                  // para no acoplar este backend a la pantalla).
}

void XpressNetTractionBackend::onXNetPower(uint8_t state) {
  // state llega en formato csXxx (ver XpressNet.h): csTrackVoltageOff
  // tiene el bit correspondiente puesto si la vía está cortada.
  TrackState prev = track_;
  track_.powerOn = !(state & csTrackVoltageOff);
  track_.emergencyStop = (state & csEmergencyStop) != 0;
  track_.shortCircuit = (state & csShortCircuit) != 0;
  track_.serviceModeActive = (state & csServiceMode) != 0;

  // PDF 2.7/2.8/2.9/2.10/2.14: TODOS estos broadcasts se disparan también
  // "si el estado fue cambiado por algún dispositivo de entrada
  // (multiMaus)", no solo cuando lo pide la app Z21 — por eso se compara
  // contra el estado anterior aquí y se avisa al núcleo vía changeCb_.
  // Antes de esto, un cambio de la MultiMaus (o un corto real en la vía)
  // nunca llegaba a los clientes Z21 conectados por WiFi.
  if (!changeCb_) return;
  if (prev.powerOn != track_.powerOn) {
    changeCb_({track_.powerOn ? TractionEventType::TrackPowerOn : TractionEventType::TrackPowerOff, 0, 0});
  }
  if (!prev.emergencyStop && track_.emergencyStop) { // solo al ACTIVARSE, ver PDF 2.14
    changeCb_({TractionEventType::EmergencyStop, 0, 0});
  }
  if (prev.shortCircuit != track_.shortCircuit && track_.shortCircuit) {
    changeCb_({TractionEventType::ShortCircuit, 0, 0});
  }
  if (!prev.serviceModeActive && track_.serviceModeActive) {
    changeCb_({TractionEventType::ProgrammingMode, 0, 0});
  }
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

  // PDF 4.4: LAN_X_LOCO_INFO se manda sin pedirlo "si el estado de la
  // locomotora fue cambiado por uno de los (otros) clientes o mandos" —
  // esto incluye tanto la MultiMaus como la confirmación real de un
  // comando que mandamos nosotros mismos (ver setLocoDrive():
  // pendingBusConfirmation). En ambos casos el núcleo Z21 necesita
  // enterarse para reenviarlo; mega_z21.ino decide a quién excluir.
  if (changeCb_) changeCb_({TractionEventType::LocoChanged, addr, 0});
}

void XpressNetTractionBackend::onTrnt(uint8_t adrHigh, uint8_t adrLow, uint8_t pos) {
  uint16_t addr = ((uint16_t)adrHigh << 8) | adrLow; // sin enmascarar, ver setTurnout()
  AccessoryState *acc = accessories_.findOrAlloc(addr);
  acc->position = static_cast<AccessoryPosition>(pos & 0x03);
  // PDF 5.3: mismo razonamiento que onLokAll() de arriba, para agujas.
  if (changeCb_) changeCb_({TractionEventType::TurnoutChanged, addr, 0});
}

void XpressNetTractionBackend::onCVResult(uint8_t cvAdr, uint8_t cvData) {
  cvPending_ = false;
  // cvAdr aquí es el mismo 'byte CV' 0-255 que se mandó a readCVMode/
  // writeCVMode (ver cvRead()/cvWrite()) — mega_z21.ino lo reconstruye
  // como CV Z21 (CVAdr_MSB=0, CVAdr_LSB=cvAdr) al construir LAN_X_CV_RESULT.
  if (changeCb_) changeCb_({TractionEventType::CvResult, cvAdr, cvData});
}

void XpressNetTractionBackend::onCVInfo(uint8_t state) {
  // Ver punto 7b de "ASUNCIONES A VALIDAR" en traction_backend_
  // xpressnet.h: el significado exacto de 'state' no está documentado
  // más allá del nombre de la función. De momento NO se interpreta como
  // NACK aquí a propósito (evitar un falso NACK antes de confirmar el
  // significado real contra hardware) — el único mecanismo de "no dejar
  // la app colgada" es el timeout de cvPending_ en poll().
  (void)state; // TODO: revisar contra hardware real y decidir si esto
               // debe traducirse a CvNack o a otra cosa.
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

void notifyCVResult(uint8_t cvAdr, uint8_t cvData) {
  if (g_activeXpressNetBackend) {
    g_activeXpressNetBackend->onCVResult(cvAdr, cvData);
  }
}

void notifyCVInfo(uint8_t State) {
  if (g_activeXpressNetBackend) {
    g_activeXpressNetBackend->onCVInfo(State);
  }
}

#endif // TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_XPRESSNET
