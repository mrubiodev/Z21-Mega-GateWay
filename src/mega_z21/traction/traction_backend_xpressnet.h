/*
 * traction_backend_xpressnet.h
 * -----------------------------
 * XpressNetTractionBackend: implementación real de ITractionBackend para
 * la v1 del roadmap (docs/Z21_EMULATOR_SPEC.md sección 11) — esclavo de
 * la Roco MultiMaus standalone por RS485/X-Bus, usando la librería
 * externa `Digital-MoBa/XpressNet` ("Arduino XpressNet Slave",
 * https://github.com/Digital-MoBa/XpressNet).
 *
 * DEPENDENCIA EXTERNA — instalar antes de compilar este backend:
 *   Arduino IDE -> Sketch -> Include Library -> Manage Libraries...
 *   buscar "XpressNet" (Philipp Gahtow) e instalar, o clonar
 *   https://github.com/Digital-MoBa/XpressNet dentro de tu carpeta
 *   Arduino/libraries. No se vendoriza dentro de este repo a propósito
 *   (es una librería de terceros con su propia licencia — "for Private
 *   use only" según su cabecera; cada quien la instala en su entorno).
 *
 * PUERTO SERIE: la propia librería, en su rama para AVR (no ESP), fuerza
 * el uso de Serial1 en cualquier Mega/Leonardo (ver XpressNet.h,
 * SERIAL_PORT_1 para __AVR_ATmega2560__) — coincide con lo ya reservado
 * en docs/Z21_EMULATOR_SPEC.md sección 2 ("Serial1 o Serial2, libres").
 * No hace falta configurarlo aquí, la librería lo decide sola.
 *
 * PENDIENTE DE HARDWARE (ver docs/Z21_EMULATOR_SPEC.md sección 12,
 * "Pendiente de
 * definir"): el pin exacto de DE/RE del MAX485 todavía no está fijado en
 * esta placa. TRACTION_XPRESSNET_DE_RE_PIN de abajo es un PLACEHOLDER —
 * verificar que no choca con el shield TFT/encoder/E-stop antes de
 * flashear con este backend activo.
 *
 * DIRECCIÓN PROPIA EN EL BUS: TRACTION_XPRESSNET_MY_ADDRESS (1-31, debe
 * ser única en el bus junto a la MultiMaus y cualquier otro cliente).
 * Ver docs/Z21_EMULATOR_SPEC.md sección 12, "gestión de asignación de dirección de
 * cliente" — de momento fija por #define, sin negociación automática.
 *
 * ASUNCIONES A VALIDAR CONTRA HARDWARE REAL antes de dar esto por
 * cerrado (documentado explícitamente, no se puede compilar ni probar
 * esta librería de terceros desde este entorno de trabajo):
 *   1) setLocoDrive(Adr_High, Adr_Low, Steps, Speed): se asume que
 *      'Speed' ya lleva el sentido en el bit7 (mismo formato que DB3 de
 *      Z21/X-Bus 0x13 "RVVVVVVV"), igual que speedByte en LocoState. Si
 *      al probar con la MultiMaus el sentido sale invertido o no aplica,
 *      revisar aquí primero (xpressnetSpeedFromZ21()).
 *   2) Código de pasos (Steps): el propio header de la librería documenta
 *      el campo `mode` de xLokSts como "0=14, 1=27, 2=28, 3=128", que NO
 *      es el mismo code que stepsCode de Z21 (0=14, 2=28, 4=128). Se
 *      convierte explícitamente en xpressnetStepsFromZ21()/
 *      z21StepsFromXpressnet() — revisar primero aquí si la MultiMaus
 *      muestra un nº de pasos distinto al configurado desde la app.
 *   3) Los grupos de funciones F0-F4/F5-F12/F13-F20/F21-F28 SÍ coinciden
 *      bit a bit entre el LocoState (formato Z21 DB4-DB7) y los campos
 *      f0/f1/f2/f3 que entrega notifyLokAll (mismo origen X-Bus para
 *      ambos protocolos) — ver conversión en el .cpp, no debería hacer
 *      falta tocarla, pero se deja documentada por si acaso.
 *   4) ACCESORIOS (agujas/señales, ver PDF sección 5): setTrntPos(Adr_
 *      High, Adr_Low, Pos) de la librería espera 'Pos' en el mismo
 *      formato de nibble bajo que ya usaba el proyecto de referencia
 *      tkoning/Z21-arduino para reenviar LAN_X_SET_TURNOUT tal cual
 *      (Z21_EMULATOR_SPEC.md sección 13): Pos = (A<<3)|P, es decir el DB2
 *      de Z21 (10Q0A00P) con el bit Q descartado y desplazado a nibble
 *      bajo — se asume que XpressNetClass reconstruye el mismo paquete
 *      X-Bus "Weichenbefehl" (0x52) a partir de ahí. El callback
 *      notifyTrnt(Adr_High, Adr_Low, Pos) que entrega la respuesta del
 *      bus se asume simétrico: se asume que su 'Pos' trae directamente
 *      el campo ZZ de LAN_X_TURNOUT_INFO en los 2 bits bajos (00=no
 *      conmutada, 01/10=salida1/2, 11=inválida) — NINGUNA de las dos
 *      asunciones está confirmada contra hardware real todavía (no hay
 *      forma de compilar/probar esta librería de terceros desde este
 *      entorno de trabajo); si al probar con la MultiMaus la posición
 *      informada no coincide con la aguja real, revisar primero aquí
 *      (setTurnout()/onTrnt() en el .cpp).
 */
#ifndef TRACTION_BACKEND_XPRESSNET_H
#define TRACTION_BACKEND_XPRESSNET_H

#include "traction_config.h"

// Todo el contenido de este header (y de su .cpp) queda vacío si NO está
// seleccionado este backend en traction_config.h — así mega_z21.ino
// puede incluirlo siempre sin forzar la dependencia de la librería
// externa <XpressNet.h> cuando se compila con el backend dummy (ver el
// motivo detallado en traction_config.h: cada .cpp del sketch es una
// unidad de compilación aparte, así que la guarda tiene que estar aquí,
// no solo en el .ino).
#if TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_XPRESSNET

#include "traction_backend.h"
#include "loco_state_store.h"
#include "accessory_state_store.h"

#ifndef TRACTION_XPRESSNET_MY_ADDRESS
#define TRACTION_XPRESSNET_MY_ADDRESS 30 // 1-31, único en el bus (ver docs/Z21_EMULATOR_SPEC.md sección 12)
#endif

#ifndef TRACTION_XPRESSNET_DE_RE_PIN
#define TRACTION_XPRESSNET_DE_RE_PIN 4 // PLACEHOLDER: confirmar contra el hardware real antes de flashear
#endif

class XpressNetTractionBackend : public ITractionBackend {
public:
  XpressNetTractionBackend();

  void begin() override;
  void poll() override;

  bool setTrackPower(bool on) override;
  void emergencyStopAll() override;
  TrackState getTrackState() const override;

  void setLocoDrive(uint16_t addr, uint8_t stepsCode, uint8_t speedByte) override;
  void setLocoFunction(uint16_t addr, uint8_t index, FunctionOp op) override;
  void setLocoFunctionGroup(uint16_t addr, FunctionGroup group, uint8_t value) override;

  void requestLocoRefresh(uint16_t addr) override;
  const LocoState *getLocoState(uint16_t addr) override;

  void setTurnout(uint16_t addr, bool output, bool activate) override;
  void requestTurnoutRefresh(uint16_t addr) override;
  const AccessoryState *getTurnoutState(uint16_t addr) override;

  // --- Puente hacia los callbacks "weak" de la librería (ver .cpp) ---
  // Públicos porque los invocan funciones libres de espacio de nombres
  // global (notifyXNetStatus, notifyLokAll, ...), NO forman parte del
  // contrato ITractionBackend ni deben llamarse desde el núcleo Z21.
  void onXNetStatus(uint8_t ledState);
  void onXNetPower(uint8_t state);
  void onLokAll(uint8_t adrHigh, uint8_t adrLow, bool busy, uint8_t steps,
                uint8_t speed, uint8_t direction, uint8_t f0, uint8_t f1,
                uint8_t f2, uint8_t f3, bool req);
  void onTrnt(uint8_t adrHigh, uint8_t adrLow, uint8_t pos);

private:
  TrackState track_;
  LocoStateStore locos_;
  AccessoryStateStore accessories_;
};

// Única instancia posible: la librería XpressNetClass usa un puntero
// estático interno ("active_object") para su propio manejador de
// interrupción de Serial, por diseño solo puede haber UN backend
// XpressNet activo a la vez en todo el firmware — igual que ya pasa con
// el driver de la pantalla TFT (ver display_driver.cpp). Los callbacks
// "weak" de espacio de nombres global (.cpp) reenvían aquí.
extern XpressNetTractionBackend *g_activeXpressNetBackend;

#endif // TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_XPRESSNET

#endif // TRACTION_BACKEND_XPRESSNET_H
