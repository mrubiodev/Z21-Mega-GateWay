/*
 * traction_backend.h
 * ------------------
 * ITractionBackend — la "capa de abstracción de backend de tracción" que
 * ya estaba prometida en AGENT.md y en docs/Z21_EMULATOR_SPEC.md sección
 * 11, ahora hecha real. Principio de inversión de dependencias (la "D"
 * de SOLID): mega_z21.ino (el núcleo del protocolo Z21) depende SOLO de
 * esta interfaz, nunca de XpressNetClass ni de ningún otro detalle de
 * bus concreto. Así:
 *   - v1 (ahora): XpressNetTractionBackend — esclavo de la Roco MultiMaus.
 *   - v2 (futuro): LocoNetTractionBackend.
 *   - v3 (futuro): backend DCC directo, la placa genera su propia señal.
 * añadir un backend nuevo es escribir una clase nueva que implemente esta
 * interfaz — no se toca ni una línea del núcleo Z21 (principio
 * abierto/cerrado, la "O" de SOLID).
 *
 * Por qué la interfaz habla en términos "de índice de función" (0-28) y
 * no en términos de bytes de grupo (F0-F4/F5-F8/...): el núcleo Z21 no
 * tiene por qué conocer cómo empaqueta cada bus sus funciones en bytes.
 * Cada backend concreto hace la conversión puertas adentro. Esto es lo
 * que de verdad desacopla el protocolo Z21 del bus físico — antes del
 * refactor, mega_z21.ino manipulaba directamente los bits de DB4/DB5/
 * DB6/DB7 de la tabla dummy, mezclando protocolo Z21 con "formato de
 * bus" en el mismo sitio.
 *
 * Todos los métodos deben ser NO bloqueantes: poll() se llama una vez
 * por vuelta de loop() del Mega, igual que el resto del sistema (parada
 * de emergencia por hardware, framing con el ESP...). Un backend real
 * (XpressNet) es intrínsecamente asíncrono: pedir un dato no lo entrega
 * al momento, lo entrega más tarde vía callback dentro de poll()/receive().
 * getLocoState() siempre devuelve la última información CONOCIDA
 * (posiblemente solo un valor por defecto si aún no ha llegado nada del
 * bus todavía), nunca bloquea esperando al bus.
 */
#ifndef TRACTION_BACKEND_H
#define TRACTION_BACKEND_H

#include "traction_types.h"

// TTNNNNNN de LAN_X_SET_LOCO_FUNCTION (PDF 4.3.1): qué hacer con la
// función indicada por índice.
enum class FunctionOp : uint8_t {
  Off = 0b00,
  On = 0b01,
  Toggle = 0b10
  // 0b11 = "no permitido" según el PDF; el núcleo Z21 lo filtra antes de
  // llamar al backend, ver mega_z21.ino.
};

// Grupos de LAN_X_SET_LOCO_FUNCTION_GROUP (PDF 4.3.2). El valor que
// acompaña a cada grupo llega ya empaquetado en el mismo orden de bits
// que espera el backend concreto (ver cada implementación para el
// detalle) — es la única concesión a "formato de bus" que se deja pasar
// tal cual, porque construirlo función a función en el núcleo Z21 solo
// para desempaquetarlo otra vez dentro del backend no aporta nada; está
// documentado grupo a grupo en cada backend concreto.
enum class FunctionGroup : uint8_t {
  F0toF4 = 1,
  F5toF8 = 2,
  F9toF12 = 3,
  F13toF20 = 4,
  F21toF28 = 5
};

class ITractionBackend {
public:
  virtual ~ITractionBackend() {}

  // Inicializa el bus físico (UART, pines DE/RE, dirección propia en el
  // bus, etc.). Se llama una vez desde setup().
  virtual void begin() = 0;

  // Se llama una vez por vuelta de loop(). NO debe bloquear nunca — debe
  // devolver el control enseguida igual que el resto del bucle principal
  // (ver AGENT.md, "Seguridad — no negociable": nada de esto puede meter
  // latencia en el camino de la parada de emergencia).
  virtual void poll() = 0;

  // Corta/activa la tracción de verdad en la vía. Devuelve true si el
  // backend pudo encolar/enviar la orden (no implica confirmación del
  // bus todavía; el estado confirmado se refleja en getTrackState()).
  virtual bool setTrackPower(bool on) = 0;

  // Parada de emergencia general (LAN_X_SET_STOP). Distinta de
  // setTrackPower(false): la vía puede seguir energizada pero con todas
  // las locos a velocidad 0 (ver PDF, diferencia entre Stop y Power Off).
  virtual void emergencyStopAll() = 0;

  // Último estado de vía conocido (nunca bloquea).
  virtual TrackState getTrackState() const = 0;

  // Envía un cambio de velocidad/sentido para una locomotora. speedByte
  // usa el mismo formato que Z21 DB3 (bit7=sentido, bits0-6=velocidad) —
  // ver traction_types.h.
  virtual void setLocoDrive(uint16_t addr, uint8_t stepsCode, uint8_t speedByte) = 0;

  // Cambia UNA función por índice (0=F0 ... 28=F28).
  virtual void setLocoFunction(uint16_t addr, uint8_t index, FunctionOp op) = 0;

  // Cambia un grupo de funciones de una vez (ver comentario de
  // FunctionGroup sobre el formato de 'value').
  virtual void setLocoFunctionGroup(uint16_t addr, FunctionGroup group, uint8_t value) = 0;

  // Pide refrescar el estado de una loco desde el bus real. En backends
  // síncronos/dummy no hace nada (el estado ya está siempre al día). En
  // backends asíncronos dispara la petición; la respuesta llega más
  // tarde y se recoge dentro de poll() — getLocoState() mientras tanto
  // sigue devolviendo el último valor conocido.
  virtual void requestLocoRefresh(uint16_t addr) = 0;

  // Último estado conocido de una locomotora (nunca null: si no existía,
  // el backend debe crear una entrada con los valores por defecto de
  // LocoState). Nunca bloquea esperando al bus.
  virtual const LocoState *getLocoState(uint16_t addr) = 0;

  // ---------------------------------------------------------------------
  // Accesorios (agujas, señales de 2 aspectos, desacopladores,
  // descarriladores biestables... ver AccessoryState en traction_types.h
  // y PDF Z21 sección 5 "Switching" para el detalle de por qué todos
  // caben en el mismo modelo).
  // ---------------------------------------------------------------------

  // Cambia la salida de un accesorio (LAN_X_SET_TURNOUT, PDF 5.2).
  // output: false = salida 1 (P=0), true = salida 2 (P=1). activate:
  // true = activar la salida (A=1), false = desactivarla (A=0) — la app
  // Z21 manda típicamente A=1 y, tras un pulso breve, A=0 para la misma
  // dirección/salida, igual que se comanda un decodificador de
  // accesorios DCC real (bobina de aguja, relé de desacoplador...). El
  // backend decide qué hacer con cada uno de los dos casos; no todos los
  // backends necesitan tratarlos distinto (ver DummyTractionBackend).
  virtual void setTurnout(uint16_t addr, bool output, bool activate) = 0;

  // Pide refrescar el estado de un accesorio desde el bus real. No-op en
  // backends síncronos/dummy, igual que requestLocoRefresh.
  virtual void requestTurnoutRefresh(uint16_t addr) = 0;

  // Último estado conocido de un accesorio (nunca null: si no existía,
  // el backend debe crear una entrada con los valores por defecto de
  // AccessoryState). Nunca bloquea esperando al bus.
  virtual const AccessoryState *getTurnoutState(uint16_t addr) = 0;
};

#endif // TRACTION_BACKEND_H
