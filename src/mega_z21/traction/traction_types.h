/*
 * traction_types.h
 * ----------------
 * Tipos de datos compartidos por TODOS los backends de tracción (XpressNet
 * hoy, LocoNet/DCC directo mañana — ver AGENT.md "Capa de abstracción
 * backend de tracción" y docs/Z21_EMULATOR_SPEC.md sección 11).
 *
 * MOTIVO de este fichero (mismo motivo que ya justificaba loco_state.h,
 * ver historial git): el IDE de Arduino autogenera los prototipos de
 * todas las funciones del .ino y los coloca justo después de los
 * #include, ANTES de cualquier struct definido más abajo en el propio
 * .ino. Cualquier tipo usado en la firma de una función del .ino tiene
 * que existir ya en ese punto, así que vive en un header incluido al
 * principio.
 *
 * DISEÑO (por qué esto ya NO es "DummyLocoState"):
 * Antes había una única tabla `dummyLocos[]` con semántica de "estado
 * simulado". Ahora LocoState es el estado de UNA locomotora tal y como lo
 * entiende el protocolo Z21 (LAN_X_GET_LOCO_INFO / SET_LOCO_DRIVE /
 * SET_LOCO_FUNCTION*, ver PDF secciones 4.2-4.4) — es la moneda común
 * entre el núcleo Z21 y CUALQUIER backend de tracción. Un backend real
 * (XpressNet) lo rellena a partir de lo que responde el bus físico; un
 * backend dummy lo rellena con lo último que la propia app mandó. El
 * núcleo Z21 (mega_z21.ino) no necesita saber cuál de los dos es.
 *
 * Los layouts de bits (f0to4 orden L/F/G/H/J, f5to12/f13to20/f21to28
 * como bit0=función más baja del grupo) son exactamente los que exige el
 * protocolo LAN_X de Z21 (PDF 4.3.1/4.3.2) — y por diseño de Roco/
 * Fleischmann coinciden bit a bit con los grupos F1/F2/F3 de X-Bus tal
 * cual los expone XpressNetClass::notifyLokAll (ver traction_backend_
 * xpressnet.cpp para el detalle grupo a grupo y qué SÍ hay que convertir).
 */
#ifndef TRACTION_TYPES_H
#define TRACTION_TYPES_H

#include <stdint.h>

// Nº de locomotoras que se recuerdan en RAM simultáneamente. Con backend
// XpressNet esto es además el límite práctico de "cuántas locos puede
// controlar esta central emulada a la vez" (memoria, no protocolo — el
// protocolo XpressNet soporta más vía slots, ver XpressNetMaster SlotMax).
#define MAX_TRACKED_LOCOS 8

// Estado de una locomotora, en el formato que usa el protocolo Z21 LAN_X
// (ver PDF 4.2-4.4). stepsCode: 0=14, 2=28, 4=128 pasos (tal cual DB2 de
// LAN_X_LOCO_INFO, NO el código crudo de X-Bus, que difiere — ver backend
// XpressNet para la conversión).
struct LocoState {
  uint16_t address = 0;   // 0 = slot libre
  uint8_t stepsCode = 4;  // 128 pasos por defecto
  uint8_t speedByte = 0x80; // bit7=sentido(1=adelante), bits0-6=velocidad; parado
  uint8_t f0to4 = 0;      // DB4: L=F0(bit4) F=F4(bit3) G=F3(bit2) H=F2(bit1) J=F1(bit0)
  uint8_t f5to12 = 0;     // DB5: F5=bit0 ... F12=bit7
  uint8_t f13to20 = 0;    // DB6: F13=bit0 ... F20=bit7
  uint8_t f21to28 = 0;    // DB7: F21=bit0 ... F28=bit7
  // true mientras se espera la primera respuesta real del bus físico para
  // esta loco (solo lo usan backends asíncronos, p.ej. XpressNet). El
  // núcleo Z21 puede usarlo para decidir si loguear "dato aún no
  // confirmado por la vía" — de momento solo informativo, no bloquea la
  // respuesta (la app espera respuesta rápida, no hacerla esperar al bus).
  bool pendingBusConfirmation = false;
};

// Estado global de la vía, independiente del backend. Se corresponde con
// los bits CS_TRACK_VOLTAGE_OFF / CS_EMERGENCY_STOP / CS_SHORT_CIRCUIT /
// CS_PROGRAMMING_MODE_ACTIVE de z21_protocol.h (buildCentralStateByte()
// en mega_z21.ino los traduce a partir de esto).
struct TrackState {
  bool powerOn = false;       // arranca en OFF, como toda central Z21 real
  bool emergencyStop = false;
  bool shortCircuit = false;
  bool serviceModeActive = false;
};

#endif // TRACTION_TYPES_H
