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
 * OJO, PREGUNTA FRECUENTE — DE/RE NO ES RX1/TX1: el UART de verdad
 * (pines 18=TX1/19=RX1 del Mega) lo reserva y maneja la propia librería
 * XpressNet en modo automático, sin que este backend tenga que
 * configurarlo (ver el párrafo de arriba). El DE/RE del módulo MAX485 es
 * OTRO pin, aparte del UART: es la línea que le dice al chip MAX485 si
 * tiene que actuar como transmisor o como receptor en el bus half-duplex
 * (en la mayoría de módulos MAX485 baratos los pines DE y RE físicos del
 * chip vienen ya unidos en la placa, así que es un único cable/pin desde
 * el Mega). La librería XpressNet es quien conmuta este pin sola
 * (HIGH al transmitir, LOW al recibir) en cuanto se le pasa en su
 * start(), este backend no tiene que tocarlo a mano en ningún sitio.
 *
 * PENDIENTE DE HARDWARE (ver docs/Z21_EMULATOR_SPEC.md sección 12,
 * "Pendiente de
 * definir"): el pin exacto de DE/RE del MAX485 todavía no está fijado en
 * esta placa. TRACTION_XPRESSNET_DE_RE_PIN de abajo es un PLACEHOLDER —
 * verificar que no choca con el shield TFT/encoder/E-stop antes de
 * flashear con este backend activo. Ya no choca con los pines nuevos de
 * input_config.h (E-stop=2, encoder A=3/B=21/botón=20): el placeholder
 * actual (pin 4) queda libre de esos, pero confirmar contra el hardware
 * real de todos modos.
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
 *   5) ACCESORIOS EXTENDIDOS (señales de más de 2 aspectos / DCCext según
 *      RCN-213, LAN_X_SET_EXT_ACCESSORY, PDF sección 5.4): esto NO es una
 *      asunción a validar, es una LIMITACIÓN CONOCIDA de esta v1 —
 *      confirmada revisando la API pública de Digital-MoBa/XpressNet
 *      (XpressNet.h en https://github.com/Digital-MoBa/XpressNet): la
 *      librería es anterior a RCN-213/DCCext y solo expone setTrntPos()
 *      para el paquete DCC "basic accessory decoder" de 2 salidas (PDF
 *      sección 5.2) — no hay ningún método para construir o enviar el
 *      paquete "extended accessory decoder" (DCCext) que necesita
 *      LAN_X_SET_EXT_ACCESSORY, ni una forma de leer del bus el último
 *      valor mandado a uno. Por tanto XpressNetTractionBackend::
 *      setExtAccessory() en esta v1 SOLO actualiza el estado en RAM (ver
 *      .cpp) — la app Z21 recibe una respuesta coherente y consistente,
 *      pero NADA sale de verdad hacia la MultiMaus/vía para accesorios
 *      extendidos. Para soportarlo de verdad hacen falta, en algún futuro
 *      backend (o una versión propia/fork de la librería), o bien
 *      construir el paquete DCCext a mano y encolarlo si la librería
 *      expone algo de bajo nivel para inyectar tramas X-Bus arbitrarias
 *      (no confirmado que lo haga), o bien esperar a que el propio
 *      proyecto Digital-MoBa/XpressNet lo incorpore. Revisar aquí primero
 *      si en el futuro se necesita cubrir señales de más de 2 aspectos
 *      de verdad sobre hardware XpressNet real.
 *   6) FUNCIONES F29-F68 (grupos 6-10 de LAN_X_SET_LOCO_FUNCTION_GROUP,
 *      ampliación Z21 FW V1.42, ver FunctionGroup en traction_backend.h):
 *      LIMITACIÓN CONOCIDA, no una asunción — Digital-MoBa/XpressNet es
 *      anterior a esta ampliación y su API pública solo expone
 *      setFunc0to4()/setFunc5to8()/setFunc9to12()/setFunc13to20()/
 *      setFunc21to28(); no hay ningún setFunc29to36() ni equivalente. El
 *      propio X-Bus clásico (Lenz) que habla esta librería con la
 *      MultiMaus tampoco define un comando de grupo para F29 en
 *      adelante — es una ampliación exclusiva del protocolo LAN de Z21,
 *      no de X-Bus. Por eso setLocoFunctionGroup()/setLocoFunction() en
 *      esta v1, para F29-F68, SOLO actualizan el LocoState en RAM (igual
 *      que setExtAccessory() en el punto 5) — la app Z21 recibe una
 *      respuesta coherente (incluido el byte extendido F29-F31 de
 *      LAN_X_LOCO_INFO, ver sendLocoInfoResponse() en mega_z21.ino), pero
 *      NADA sale hacia la MultiMaus/vía para estas funciones todavía.
 *      Coherente además con el propio PDF (remark D de la tabla 4.3.2):
 *      incluso en una Z21 real, F32-F68 tampoco llevan confirmación de
 *      vuelta al cliente LAN, así que esta limitación no se nota desde
 *      el punto de vista del protocolo — solo desde el punto de vista de
 *      "¿de verdad se mueve algo en la vía?", que aquí es que no.
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
#include "ext_accessory_state_store.h"

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
  void purgeLoco(uint16_t addr) override;

  void setTurnout(uint16_t addr, bool output, bool activate) override;
  void requestTurnoutRefresh(uint16_t addr) override;
  const AccessoryState *getTurnoutState(uint16_t addr) override;

  // Ver punto 5 de "ASUNCIONES A VALIDAR" más arriba: en esta v1 solo
  // actualizan el estado en RAM, no hay forma de mandar DCCext de verdad
  // por el bus con esta librería.
  void setExtAccessory(uint16_t rawAddr, uint8_t state) override;
  void requestExtAccessoryRefresh(uint16_t rawAddr) override;
  const ExtAccessoryState *getExtAccessoryState(uint16_t rawAddr) override;

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
  ExtAccessoryStateStore extAccessories_;
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
