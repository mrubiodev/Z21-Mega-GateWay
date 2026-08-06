# Z21_EMULATOR_SPEC — Emulador Z21 sobre Mega2560 + ESP8266

## 1. Objetivo

Sustituir la Z21 real por un emulador propio que:
- Habla **Z21 LAN protocol** (UDP, puerto 21105) hacia la app Z21 / Rocrail / JMRI por WiFi.
- Habla **XpressNet (X-Bus)** por RS485 hacia la **Roco MultiMaus** (standalone, con booster propio), actuando como cliente en el bus — la MultiMaus sigue siendo la maestra que genera la señal DCC real.
- Reemplaza por completo el Arduino Uno anterior y el router WiFi externo: una sola placa hace de AP/servidor y de puente XpressNet.

Base de partida: **`Digital-MoBa/DCCInterfaceMaster`** ("Arduino Z21 Digital Zentrale"), mantenido por Philipp Gahtow con soporte activo para ESP8266/ESP32 — mucho más completo que la rama antigua limitada al Arduino Uno (ver sección 13, Referencias). Aprovechando los 8KB de SRAM del Mega2560, aquí se apunta a implementar el protocolo **completo**.

## 2. Reparto de hardware

**Placa base confirmada**: combo "Mega +WiFi R3 ATmega2560+ESP8266" (32Mb flash, CH340G, 8 DIP switches + botón "Mode" para flashear el ESP). El enlace Mega↔ESP de esta placa **no es libre**: solo puede ir por Serial0 (pines 0/1) o Serial3 (pines 14/15), seleccionado por DIP switch — no por cualquier UART como se planteó inicialmente.

| Función | Chip | Notas |
|---|---|---|
| Servidor UDP Z21 (WiFi) | ESP8266 | AP propio (fallback) o cliente de la WiFi doméstica; puerto 21105 |
| Parseo Z21 ↔ XpressNet, estado del sistema | ATmega2560 | Núcleo del emulador |
| Enlace ESP8266 ↔ Mega | **Serial3** (pines 14 RX3 / 15 TX3), vía DIP switch en modo MCU↔ESP | Framing `[TYPE(1)][LEN(1)][payload]` — TYPE distingue datagrama Z21 real de heartbeat de diagnóstico interno; deja Serial0 libre para USB/debug |
| Enlace Mega ↔ MultiMaus | RS485 (MAX485/MAX481) sobre **Serial1** o **Serial2** (libres) | 62.5 kBaud, 9 bits de datos, sin paridad, 1 stop bit; pin DE/RE para half-duplex |
| Programación/debug Mega | **Serial0** (USB, CH340G) | Libre siempre que el DIP switch no lo desvíe al ESP |

## 3. Alcance funcional — Z21 LAN protocol completo

### v1 (imprescindible para MultiMaus + app Z21 básica)
- `LAN_GET_SERIAL_NUMBER`, `LAN_GET_HWINFO`, `LAN_LOGOFF`
- `LAN_X_GET_VERSION`, `LAN_X_GET_STATUS`, `LAN_X_SET_TRACK_POWER_OFF/ON`
- `LAN_SYSTEMSTATE_GETDATA` / `LAN_SYSTEMSTATE_DATACHANGED`
- `LAN_SET_BROADCASTFLAGS` / `LAN_GET_BROADCASTFLAGS`
- `LAN_X_GET_LOCO_INFO`, `LAN_X_SET_LOCO_DRIVE` (14/28/128 pasos), `LAN_X_SET_LOCO_FUNCTION`, `LAN_X_SET_LOCO_FUNCTION_GROUP`
- `LAN_X_SET_LOCO_E_STOP`, `LAN_X_SET_STOP` (parada de emergencia general)
- `LAN_X_GET_FIRMWARE_VERSION`

### v2 (ampliación hacia el 100%)
- `LAN_X_CV_READ`, `LAN_X_CV_WRITE`, `LAN_X_CV_POM_WRITE_BYTE/BIT`
- ~~`LAN_X_GET_TURNOUT_INFO`, `LAN_X_SET_TURNOUT`~~ — **implementado (ver sección 5c)**
- `LAN_X_SET_EXT_ACCESSORY` (señales de más de 2 aspectos, PDF sección 5.4) — pendiente
- `LAN_GET_LOCOMODE` / `LAN_SET_LOCOMODE`
- `LAN_RMBUS_GETDATA` / `LAN_RMBUS_DATACHANGED` (si se añaden detectores de ocupación)
- `LAN_X_PURGE_LOCO`

## 4. Modelo de estado en RAM

Por cada locomotora activa:
- Dirección (14 bits)
- Velocidad + sentido de marcha
- Formato de pasos (14/28/128)
- Funciones F0–F28 (bitfield de 32 bits)

Por cada accesorio activo (agujas, señales de 2 aspectos, desacopladores,
descarriladores biestables — ver sección 5c):
- Dirección (16 bits, sin enmascarar)
- Posición conocida (no conmutada / salida 1 / salida 2 / inválida)

Estado global:
- Track power on/off
- Modo programación (CV) activo/inactivo
- Corriente y voltaje de vía (si se puede leer del bus XpressNet)
- Broadcast flags activos por cliente UDP conectado

Con ~50 locomotoras simultáneas el consumo de RAM es del orden de pocos KB — holgado en el Mega2560.

## 5. Prioridad crítica: reconocimiento como Z21 legítima

Antes de avanzar con tracción/XpressNet, el objetivo nº1 es que la app Z21 oficial reconozca el dispositivo como una central legítima (no dar error de "central no válida"). Handshake mínimo que la app comprueba al conectar, todos deben responder de forma coherente entre sí:

1. `LAN_GET_SERIAL_NUMBER` — nunca 0
2. `LAN_GET_HWINFO` — `HwType = 0x00000201` ("black Z21", variante 2013, la más compatible); FW version coherente con el resto
3. `LAN_X_GET_VERSION` / `LAN_X_GET_STATUS` — canal X-Bus, formato distinto al de arriba
4. `LAN_GET_CODE` — responder `Z21_NO_LOCK = 0x00` (sin funciones bloqueadas)

**MVP de validación**: antes de tocar XpressNet o pantalla, montar un firmware mínimo que solo responda estos 4 comandos y probarlo con la app Z21 real en el móvil, para confirmar que el reconocimiento funciona antes de construir el resto encima.

## 5b. Diagnóstico del enlace Mega↔ESP

Al probar el primer MVP (WiFi + web funcionando pero la app Z21 sin encontrar el dispositivo), se detectó que hacía falta visibilidad de si el Mega está vivo y respondiendo — sin eso, un fallo de DIP switch/baudios/cableado en el enlace Serial3 es indistinguible de un bug de protocolo.

- **Heartbeat**: el Mega manda cada 1s un frame de diagnóstico (uptime, tiempo de ciclo medio/máximo, RAM libre, contadores de frames Z21 OK/error). El ESP lo usa para saber si el Mega está vivo (timeout 3s sin heartbeat = "sin respuesta") y lo muestra en la web de configuración.
- **Watchdog**: el Mega tiene un watchdog hardware (timeout 4s) que lo autorresetea si `loop()` se cuelga, en vez de quedarse colgado en silencio.
- **Framing actualizado**: el enlace Mega↔ESP pasa de `[LEN][payload]` a `[TYPE(1)][LEN(1)][payload]`, donde TYPE distingue un datagrama Z21 real de un frame de heartbeat (que nunca sale por WiFi).

## 5c. Accesorios: agujas, señales, desacopladores, descarriladores

Primer soporte de conmutación de accesorios (`LAN_X_GET_TURNOUT_INFO` /
`LAN_X_SET_TURNOUT` / `LAN_X_TURNOUT_INFO`, PDF sección 5 "Switching").

- **Un único modelo para todo**: el protocolo Z21 no distingue tipos de
  accesorio — una aguja, una señal de 2 aspectos (rojo/verde) o un
  desacoplador/descarrilador biestable son, a nivel de LAN_X, el mismo
  "decodificador de accesorios DCC de 2 salidas" (salida 1 / salida 2).
  El firmware refleja esto con un único `AccessoryState` (dirección de
  16 bits + posición conocida) y unos únicos métodos de la capa de
  abstracción de tracción (`setTurnout()`, `requestTurnoutRefresh()`,
  `getTurnoutState()`), reutilizados igual por el backend XpressNet que
  por el dummy.
- **Direccionamiento**: a diferencia de las locomotoras (14 bits útiles,
  `Adr_MSB & 0x3F`), la dirección de accesorio usa los 16 bits completos
  de `FAdr_MSB`/`FAdr_LSB` sin enmascarar (PDF sección 5.1). Por eso la
  dirección `0` es válida para un accesorio (a diferencia de una loco), y
  la tabla en RAM usa `0xFFFF` como marcador de slot libre.
- **Formato de `LAN_X_SET_TURNOUT`**: `DB2 = 10Q0A00P` — `A` activa/
  desactiva la salida seleccionada, `P` elige salida 1/2, `Q` (desde Z21
  FW V1.24) pide encolar el comando en vez de ejecutarlo ya. `Q` se
  ignora por ahora: no hay cola de accesorios implementada, todo se
  ejecuta de inmediato (comportamiento `Q=0`, compatible con versiones
  anteriores del protocolo).
- **Backend XpressNet**: usa `XpressNet.setTrntPos()`/`getTrntInfo()` y
  el callback `notifyTrnt()` de la librería `Digital-MoBa/XpressNet`. La
  conversión de formato entre el `DB2` de Z21 y el `Pos` que espera la
  librería está basada en el proyecto de referencia `tkoning/Z21-arduino`
  (sección 13) pero **sin confirmar todavía contra hardware real** — ver
  el detalle completo de la asunción en `traction_backend_xpressnet.h`
  (punto 4 de "ASUNCIONES A VALIDAR").
- **Pendiente (no v1 de accesorios)**: señales de más de 2 aspectos
  necesitan `LAN_X_SET_EXT_ACCESSORY` (PDF sección 5.4, DCC "extended
  accessory decoder package format") — comando distinto, con su propio
  formato de datos; no reutiliza `AccessoryState` tal cual. Tampoco hay
  todavía forma de que la app Z21 muestre nombres/tipos de accesorio (la
  propia Z21 real tampoco lo hace por LAN — eso vive en el software de
  control, p.ej. Rocrail/JMRI, no en el protocolo).

## 6. Conectividad WiFi (ESP8266)

- Arranque en modo **STA** (cliente) contra la red WiFi guardada (credenciales en flash del ESP8266)
- Si no conecta en un tiempo límite → fallback automático a modo **AP propio** (tipo `Z21_XXXXXX`), IP fija `192.168.0.111` (la que espera por defecto la app Z21)
- Servidor Z21 UDP (puerto 21105) disponible en ambos modos

## 7. Servidor web de configuración

Servido desde el ESP8266, disponible tanto en modo STA como en modo AP:
- SSID/contraseña de la red a la que unirse
- Estado actual: modo (STA/AP), IP, RSSI, nº de clientes Z21 conectados
- Configuración de la dirección de cliente XpressNet
- Reinicio / aplicar cambios

## 8. Sniffer de depuración (fase 2)

- **No está activo de forma permanente** — se activa bajo demanda para una prueba puntual, con parada manual o timeout automático, para no afectar al rendimiento normal ni al timing estricto del polling XpressNet
- Captura tramas de ambos lados: Z21 (UDP) y XpressNet (RS485), con timestamp relativo, dirección (entrada/salida) y bytes en hex
- Se muestra por **web vía WebSocket** (no por puerto serie) — buffer asíncrono para no bloquear el bucle principal
- Formato de línea de log: `[12.345s] Z21→ DataLen=0x08 Header=0x40 Data=[A2 00 00 A2]`

## 9. Pantalla local del gateway (shield 3.5" TFT LCD en el Mega)

- Shield estándar Uno/Mega (bus paralelo de 8 bits + control por A0-A5), sin panel táctil — librería **MCUFRIEND_kbv**
- Navegación por **encoder rotativo con pulsador** (librería tipo Encoder de PJRC)
- **Botón de parada de emergencia**: interrupción hardware dedicada, dispara `LAN_X_SET_STOP` directamente, independiente del bucle de menú, para respuesta instantánea
- Funciones en pantalla: control de locomotoras y accesorios, configuración del dispositivo sin depender de la web, estado WiFi/track power/corriente-voltaje, indicador de sniffer activo
- **Tarjeta SD del shield**: registro de datos/log de sesión, y base de datos de locomotoras (CSV tipo `id,nombre,direccion,pasos,notas`)

## 10. Satélites WiFi (fase posterior, no v1)

- Mando de mano: **Wemos D1 mini (ESP8266) + pantalla táctil Lolin TFT-2.4** (controlador ILI9341, librería TFT_eSPI)
- Actúa como un cliente Z21 UDP más en la red — el gateway no necesita lógica especial para distinguirlo
- UI: selector de loco, velocidad, sentido, funciones F0-F12, agujas/accesorios

## 11. Roadmap de backends de tracción

Para que el bus "de subida" sea intercambiable sin reescribir el core Z21, se diseña desde v1 una **capa de abstracción de backend de tracción** (interfaz común tipo `sendLocoCommand()` / `getTrackStatus()`):
- **v1**: backend XpressNet — esclavo de la Roco MultiMaus (maestra, con booster propio)
- **v2**: backend LocoNet — requiere transceptor LocoNet dedicado (bus single-wire ~14V) además del RS485 existente
- **v3**: backend DCC directo — la propia placa se convierte en central Z21 completa, generando la señal DCC y con su propio booster (puente H)

## 12. Pendiente de definir
- ~~Mapeo Mega↔ESP~~ resuelto (Serial3 vía DIP switch, ver sección 2)
- Pin exacto para DE/RE del MAX485 en Serial1/Serial2 (a elegir cuál de los dos)
- Gestión de asignación de dirección de cliente en el bus XpressNet (quién la asigna al arrancar)
- Estrategia de reconexión WiFi del ESP8266 sin perder estado del Mega
- Mapeo de pines del shield 3.5" TFT + encoder + botón E-stop en el Mega (verificar que no choquen con Serial1/2/3)

## 13. Referencias

**Base de código recomendada (más completa que la rama de Uno):**
- `Digital-MoBa/DCCInterfaceMaster` — "Arduino Z21 Digital Zentrale", mantenido por Philipp Gahtow (autor original) hasta 2021+; estado completo de locos, F0-F28, CV POM, generación DCC con Timer1/Timer2, soporte Railcom, fixes de timing específicos para ESP8266/ESP32
- Wiki: "Z21 Arduino Zentrale (ESP32)" en pgahtow.de — precedente documentado de correr esto como central completa en ESP32, con test de cliente LocoNet exitoso (relevante para el roadmap v2/v3)
- `tkoning/Z21-arduino` — específico para Roco 10764 + multiMOUSE, referencia del interfaz RS485 (Waveshare) hacia el socket Slave de la central Roco

**Referencias descartadas / solo como contexto histórico:**
- pgahtow.de — Z21 Slave am XpressNet / XpressNET client library (base original, limitada en RAM en Uno)
- `kmzbrnoI/z21-arduino` — fork para Arduino UNO; el propio README reconoce que no es un emulador Z21 completo por falta de RAM (2KB en el Uno)
