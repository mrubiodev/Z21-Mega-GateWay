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
  // Grupos 6-10 (PDF 4.3.2, ampliación Z21 FW V1.42): mismo orden de
  // bits que la tabla del PDF (bit0 = función más baja del grupo). Solo
  // f29to36 llega a viajar de vuelta a la app (bits 0-2 = F29-F31, ver
  // sendLocoInfoResponse() en mega_z21.ino y el comentario de
  // FunctionGroup en traction_backend.h) — f37to44 en adelante se guardan
  // igual aquí por si algún backend futuro los necesita, pero el PDF es
  // explícito en que NO hay feedback de vuelta al cliente LAN para esos.
  uint8_t f29to36 = 0;
  uint8_t f37to44 = 0;
  uint8_t f45to52 = 0;
  uint8_t f53to60 = 0;
  uint8_t f61to68 = 0;
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

// Nº de accesorios (agujas, señales de 2 aspectos, desacopladores,
// descarriladores biestables...) que se recuerdan en RAM simultáneamente.
// El PDF oficial (sección 5, "Switching") es explícito en que el mismo
// comando sirve para "una aguja (o cualquier función de conmutación)" —
// no hay un tipo "aguja" distinto de "señal" a nivel de protocolo Z21
// LAN_X_GET/SET_TURNOUT, ambos son un decodificador de accesorios DCC de
// 2 salidas. Señales de más de 2 aspectos necesitan LAN_X_SET_EXT_
// ACCESSORY (PDF sección 5.4) — TODO, no implementado todavía (ver
// AGENT.md).
#define MAX_TRACKED_ACCESSORIES 16

// ZZ de LAN_X_TURNOUT_INFO (PDF sección 5.3): posición conocida de un
// accesorio. NotSwitched es el valor inicial antes de que llegue ningún
// comando o confirmación del bus para esa dirección.
enum class AccessoryPosition : uint8_t {
  NotSwitched = 0b00, // aún no se ha conmutado desde el arranque
  Output1 = 0b01,      // conmutado a la salida 1 (P=0 en LAN_X_SET_TURNOUT)
  Output2 = 0b10,      // conmutado a la salida 2 (P=1)
  Invalid = 0b11        // posición inválida (p.ej. ambas salidas activas a la vez)
};

// Estado de UN accesorio, en el formato que usa el protocolo Z21 LAN_X
// (ver PDF 5.1-5.3). address usa el rango completo de 16 bits sin
// enmascarar (a diferencia de LocoState::address, ver PDF sección 5.1:
// "Function address = (FAdr_MSB << 8) + FAdr_LSB", sin el "& 0x3F" que sí
// lleva la dirección de loco en 4.4) — por eso 0 SÍ es una dirección de
// accesorio válida, y el "slot libre" se marca con 0xFFFF en vez de 0
// (ver accessory_state_store.h).
struct AccessoryState {
  uint16_t address = 0xFFFF; // 0xFFFF = slot libre
  AccessoryPosition position = AccessoryPosition::NotSwitched;
};

// Nº de accesorios EXTENDIDOS (señales de más de 2 aspectos, decodificadores
// DCCext según RCN-213) que se recuerdan en RAM simultáneamente. Tabla
// separada de MAX_TRACKED_ACCESSORIES a propósito: son dos espacios de
// direcciones DISTINTOS a nivel de protocolo Z21 (ver más abajo), así que
// mezclar ambos en una sola tabla confundiría direcciones que en realidad
// no tienen relación entre sí.
#define MAX_TRACKED_EXT_ACCESSORIES 8

// Estado de UN accesorio extendido, en el formato que usa LAN_X_SET_EXT_
// ACCESSORY / LAN_X_EXT_ACCESSORY_INFO (PDF sección 5.4-5.6, "extended
// accessory decoder package format" DCCext, ver también RCN-213).
//
// DIFERENCIA CLAVE con AccessoryState (turnouts normales, sección 5.1-5.3):
// - Direccionamiento: aquí 'address' es la RawAddress según RCN-213 tal
//   cual — el primer decodificador extendido tiene RawAddress=4 (se
//   muestra como "dirección 1" en las UIs de usuario), SIN la conversión
//   a puerto/salida (FAdr>>2, etc.) que sí aplica la sección 5 a los
//   turnouts normales de 2 salidas. Son direcciones de espacios distintos
//   aunque ambas quepan en 16 bits — no intercambiables entre sí.
// - Estado: no es una posición de 2 bits (ZZ), sino un byte completo
//   DDDDDDDD (0-255) que se transmite tal cual al decodificador en el
//   paquete DCCext. Su significado concreto (aspecto de señal, tiempo de
//   activación de una aguja con "switching time"...) lo decide el
//   decodificador receptor, no el protocolo Z21 — ver PDF 5.4 para los
//   dos casos de referencia documentados (Z21 switch/signal DECODER).
struct ExtAccessoryState {
  uint16_t address = 0xFFFF; // RawAddress; 0xFFFF = slot libre (0 SÍ es
                              // válida en RCN-213, igual que en AccessoryState)
  uint8_t state = 0;         // DDDDDDDD: último valor mandado/conocido
  // false hasta que llega el primer LAN_X_SET_EXT_ACCESSORY para esta
  // dirección (o una confirmación real del bus, si el backend algún día
  // puede leerla) — controla el byte "Status" de LAN_X_EXT_ACCESSORY_INFO
  // (PDF 5.6: 0x00=Data Valid, 0xFF=Data Unknown). Antes del primer SET,
  // un GET_EXT_ACCESSORY_INFO debe poder reportar honestamente "no lo sé
  // todavía" en vez de inventar un 0x00 (Stop) que nadie mandó de verdad.
  bool hasData = false;
};

#endif // TRACTION_TYPES_H
