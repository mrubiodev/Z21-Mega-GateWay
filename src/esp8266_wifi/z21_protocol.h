/*
 * z21_protocol.h
 * --------------
 * Constantes del protocolo Z21 LAN compartidas conceptualmente entre los
 * dos sketches (esp8266_wifi y mega_z21). Como son dos toolchains/cores
 * distintos (ESP8266 vs AVR), este archivo NO se enlaza automáticamente
 * entre ambos: hay que mantener una copia igual en cada sketch, o copiar
 * este archivo dentro de cada carpeta antes de compilar. Este es el
 * archivo "fuente de verdad" del repo.
 *
 * Ver docs/Z21_EMULATOR_SPEC.md sección 5 (handshake de reconocimiento).
 */

#ifndef Z21_PROTOCOL_H
#define Z21_PROTOCOL_H

// Puerto UDP del servidor Z21
#define Z21_UDP_PORT 21105

// HwType (LAN_GET_HWINFO) — "black Z21", variante 2013, la más compatible.
// Evitar 0x00000204 (z21 start): activa restricciones de esa versión.
#define D_HWT_Z21_NEW 0x00000201UL

// LAN_GET_CODE — nivel de bloqueo de funciones
#define Z21_NO_LOCK 0x00 // sin funciones bloqueadas

// Headers de comandos usados en el handshake mínimo (fase 1)
#define LAN_GET_SERIAL_NUMBER 0x10
#define LAN_GET_HWINFO 0x1A
#define LAN_LOGOFF 0x30
#define LAN_X_GET_VERSION 0x40 // dentro de LAN_X, canal X-Bus
#define LAN_X_GET_STATUS 0x40  // dentro de LAN_X, canal X-Bus
#define LAN_GET_CODE 0x18

// Headers de la secuencia de conexión/login (PDF oficial, secciones
// 2.16-2.19). IMPORTANTE: el propio PDF dice literalmente que "el login
// se hace de forma implícita con el primer comando del cliente (p.ej.
// LAN_SYSTEM_STATE_GETDATA...)" — es decir, la app puede apoyarse en
// CUALQUIERA de estos para decidir si está hablando con una central
// válida, no solo en LAN_GET_SERIAL_NUMBER/HWINFO.
#define LAN_SET_BROADCASTFLAGS 0x50   // request: 4 bytes de flags, sin respuesta
#define LAN_GET_BROADCASTFLAGS 0x51   // request sin datos; reply: 4 bytes de flags
#define LAN_SYSTEMSTATE_DATACHANGED 0x84 // reply a GETDATA (o broadcast si el flag 0x00000100 está activo)
#define LAN_SYSTEMSTATE_GETDATA 0x85     // request sin datos

// Bitmask de CentralState / CentralStateEx dentro de SystemState (ver
// LAN_SYSTEMSTATE_DATACHANGED más abajo). Mismos bits que en el status
// dummy ya usado por handleXGetStatus() (X-Header 0x62/0x22).
#define CS_EMERGENCY_STOP 0x01
#define CS_TRACK_VOLTAGE_OFF 0x02
#define CS_SHORT_CIRCUIT 0x04
#define CS_PROGRAMMING_MODE_ACTIVE 0x20

// SystemState.Capabilities (PDF oficial sección 2.18, byte 15, definido
// desde Z21 FW V1.42). Solo se listan los bits que realmente podemos
// declarar con honestidad en esta versión dummy (DCC + comandos de
// tracción por LAN); el resto (MM, RailCom, accesorios, detectores) NO se
// marca porque no hay backend real para ellos todavía.
#define CAP_DCC 0x01
#define CAP_LOCO_CMDS 0x10

// -----------------------------------------------------------------------
// Framing interno ESP<->Mega:
//   [SYNC0(1)=0xAA][SYNC1(1)=0x55][TYPE(1)][LEN(1)][payload(LEN)][CHK(1)]
//
// CHK = XOR de TYPE, LEN y todos los bytes de payload (NO incluye los
// bytes de sync). Sirve para detectar corrupción del frame en sí — es
// distinto del checksum del protocolo Z21 (ese va dentro del payload).
//
// Los bytes de sync existen para poder RESINCRONIZAR: si se pierde o
// corrompe un byte en el enlace físico, sin marcador de sync la máquina
// de estados queda desalineada para siempre (cada byte futuro se
// interpreta con el offset equivocado) — esto es justo lo que causaba
// frames "type=0x01 len=0x00" repetidos con datos reales de por medio.
// Con sync + checksum, en cuanto un frame falla el checksum se descarta
// y se vuelve a buscar la secuencia 0xAA 0x55 en el flujo de bytes.
//
// TYPE distingue el tipo de frame:
//   FRAME_TYPE_Z21       datagrama Z21 real (tal cual llega/sale por WiFi)
//   FRAME_TYPE_HEARTBEAT diagnóstico interno Mega->ESP, nunca sale por WiFi
//   FRAME_TYPE_HELLO     Mega->ESP, "estoy listo, pásame la info de red"
//   FRAME_TYPE_NET_INFO  ESP->Mega, info de red (ver NetInfo más abajo)
//   FRAME_TYPE_SYNC_ACK  Mega->ESP, "info de red recibida, ya puedes
//                        levantar el servicio Z21 (UDP)"
// Ver AGENT.md, sección "Diagnóstico" y "Sincronización inicial".
// -----------------------------------------------------------------------
#define LINK_SYNC_BYTE_0 0xAA
#define LINK_SYNC_BYTE_1 0x55

#define FRAME_TYPE_Z21 0x01
#define FRAME_TYPE_HEARTBEAT 0x02
#define FRAME_TYPE_HELLO 0x03
#define FRAME_TYPE_NET_INFO 0x04
#define FRAME_TYPE_SYNC_ACK 0x05

// -----------------------------------------------------------------------
// Sincronización inicial Mega<->ESP (antes de levantar el servicio Z21)
// -----------------------------------------------------------------------
// El Mega manda HELLO cada SYNC_HELLO_INTERVAL_MS hasta que el ESP responde
// con NET_INFO; el Mega confirma con SYNC_ACK y a partir de ahí el ESP abre
// el UDP Z21 y empieza a reenviar tráfico real. Si no se completa en
// SYNC_TIMEOUT_MS, el Mega sigue igualmente en "modo degradado" (sin info
// de red, pero funcional) para no bloquearse para siempre si algo falla.
#define SYNC_HELLO_INTERVAL_MS 300
#define SYNC_TIMEOUT_MS 8000

// NetInfo payload (todo lo manda el ESP en FRAME_TYPE_NET_INFO), desde v0.9:
//   modo(1) | ip(4, orden de bytes normal, ip[0]=primer octeto) |
//   gateway(4, mismo orden que ip; en modo AP es la propia IP del ESP,
//   ya que el ESP es la puerta de enlace de sus clientes) |
//   mac(6, la MAC efectivamente aplicada a la interfaz -- personalizada si
//   hay una guardada, si no la MAC por defecto 84:2B:BC, ver esp8266_wifi.ino) |
//   ssidLen(1) | ssid(ssidLen, sin terminador nulo)
#define NET_INFO_MODE_STA 0x00 // conectado a una red WiFi existente
#define NET_INFO_MODE_AP 0x01  // modo AP propio (fallback), ssid = el del AP
#define NET_INFO_MAC_LEN 6
#define NET_INFO_SSID_MAXLEN 32

// -----------------------------------------------------------------------
// Versionado interno del proyecto (NO es la versión que se declara a la
// app Z21 en LAN_GET_HWINFO — esto es solo para nosotros, para saber qué
// firmware está cargado en cada chip). Subir el número al hacer cambios
// significativos en cada sketch.
// -----------------------------------------------------------------------
#define MEGA_FW_VERSION_MAJOR 0
#define MEGA_FW_VERSION_MINOR 8
#define ESP_FW_VERSION_MAJOR 0
#define ESP_FW_VERSION_MINOR 9

// Payload del heartbeat (17 bytes, todo little-endian):
//   uptimeMs (4) | cycleAvgUs (4) | cycleMaxUs (2) | freeRam (2) |
//   framesRxOk (1) | framesRxBad (1) | fwVersionMajor (1) |
//   fwVersionMinor (1) | statusCode (1)
#define HEARTBEAT_PAYLOAD_LEN 17
#define HEARTBEAT_INTERVAL_MS 1000
// Si el ESP no recibe un heartbeat en este tiempo, considera el Mega caído
#define HEARTBEAT_TIMEOUT_MS 3000

// Códigos de estado del Mega — también se reflejan en el LED pin 13 como
// parpadeo de N veces seguido de una pausa larga (N = código). Ver
// AGENT.md, sección "Diagnóstico", para el significado de cada uno.
#define STATUS_OK 1                // parpadeo normal, todo bien
#define STATUS_NO_FRAMES_EVER 2    // nunca ha llegado un datagrama Z21 válido
#define STATUS_BAD_FRAMES 3        // hay frames Z21 corruptos/mal formados
#define STATUS_WATCHDOG_RECOVERED 4 // el último arranque fue un reset del watchdog
#define STATUS_SYNCING 5           // handshake inicial con el ESP en curso (ver Sincronización)


// -----------------------------------------------------------------------
// Historial de cambios de este header compartido (recordar replicar
// cualquier edición en las 3 copias: src/shared, src/mega_z21, src/esp8266_wifi)
// -----------------------------------------------------------------------
//   - v0.4 (ver AGENT.md): framing [SYNC][TYPE][LEN][payload][CHK] +
//     handshake HELLO/NET_INFO/SYNC_ACK + heartbeat + STATUS_*.
//   - v0.5 (2026-08-02): MEGA_FW_VERSION_MINOR 4->5 al incorporar el
//     módulo de pantalla (display_*.h/.cpp en src/mega_z21/) — no hay
//     constantes nuevas en este header, la pantalla consume STATUS_* y
//     NET_INFO_MODE_* ya existentes.
//   - v0.6 (2026-08-03): nuevas constantes LAN_SET/GET_BROADCASTFLAGS,
//     LAN_SYSTEMSTATE_GETDATA/DATACHANGED y bitmask CS_* (PDF oficial
//     secciones 2.16-2.19). La app Z21 marcaba la central como "de otro
//     fabricante"; estos tres comandos forman parte de la secuencia
//     estándar de login/conexión y no estaban implementados en absoluto
//     en mega_z21.ino (ver handleGetBroadcastFlags/handleSystemStateGetData).
//   - v0.7 (2026-08-03): ESP_FW_VERSION_MINOR 4->5. Sin constantes nuevas
//     en este header; cambios solo en esp8266_wifi.ino: log de eventos con
//     niveles INFO/WARN/ERROR (filtro y export JSON en /log), sniffer
//     reescrito para registrar tramas ya decodificadas en tiempo real y en
//     ambos sentidos (antes solo Mega->ESP y solo bytes crudos) con
//     reconocimiento de comandos Z21 conocidos, y MAC de red configurable
//     por EEPROM/web (ver applyCustomMacIfSet()) para evitar colisiones de
//     MAC con otros equipos de la red.
//   - v0.8 (2026-08-03): ESP_FW_VERSION_MINOR 5->6. Sin constantes nuevas
//     en este header; cambio solo en esp8266_wifi.ino: mientras no haya
//     una MAC personalizada guardada, ya no se deja la MAC de fábrica del
//     chip -- se genera y aplica siempre una MAC con el prefijo (OUI) fijo
//     84:2B:BC, propio de nuestra red (generateDefaultMac(), llamada desde
//     applyMac(), antes applyCustomMacIfSet()). La sugerencia del botón
//     "Generar MAC aleatoria" del formulario (generateRandomLocalMac())
//     tambien se cambió para mantener siempre ese mismo prefijo 84:2B:BC,
//     aleatorizando solo los 3 bytes finales -- toda MAC que generamos
//     nosotros (por defecto o sugerida) tiene ese formato reconocible. El
//     campo `mac` del formulario web sigue permitiendo escribir a mano
//     cualquier otra MAC, incluso con otro prefijo; vacío = vuelve a la
//     MAC por defecto 84:2B:BC.
//   - v0.9 (2026-08-03): MEGA_FW_VERSION_MINOR 6->7, ESP_FW_VERSION_MINOR
//     6->7. Nueva constante NET_INFO_MAC_LEN. Payload de NetInfo ampliado:
//     antes el ESP solo mandaba el byte de modo (el resto del formato
//     documentado -- ip/ssid -- estaba definido aqui pero nunca se llegaba
//     a rellenar en esp8266_wifi.ino, era codigo muerto). Ahora
//     buildAndSendNetInfo() manda el payload completo: modo, IP local,
//     IP de la puerta de enlace (gateway) y la MAC efectiva de la interfaz,
//     ademas del SSID que ya se documentaba. handleNetInfo() en
//     mega_z21.ino se actualizo a la vez para leer gateway y MAC (antes
//     ignoraba todo lo que no fuera modo/ip/ssid) y los dos quedan
//     registrados en el log de comunicacion del Mega (ver displayLogf en
//     handleNetInfo()). Actualizar los dos sketches a la vez si se vuelve
//     a tocar este formato, o el Mega leera bytes con el offset equivocado.
//   - v0.10 (2026-08-03): MEGA_FW_VERSION_MINOR 7->8. Sin constantes
//     nuevas en este header; cambio solo en el modulo de pantalla del Mega
//     (display_types.h, display_status_panel.cpp): la fila de red de la
//     cabecera ahora pinta modo+IP+MAC juntos en una sola linea (antes
//     solo modo+IP); el gateway y el SSID siguen sin caber ahi y se quedan
//     solo en el log. No afecta a esp8266_wifi.ino ni al formato NetInfo
//     por cable (ya llevaba la MAC desde v0.9, solo que el Mega no la
//     pintaba todavia).
//   - v0.11 (2026-08-03): ESP_FW_VERSION_MINOR 7->8. Sin constantes nuevas
//     en este header; cambio solo en esp8266_wifi.ino: ahora se pueden
//     guardar hasta 3 redes WiFi (SSID+password) en vez de solo una --
//     connectWiFi() las prueba en orden y solo cae a modo AP si ninguna
//     conecta. Ademas, estando en modo AP, se reintenta conectar a alguna
//     de las guardadas cada WIFI_STA_RETRY_INTERVAL_MS (antes era un TODO
//     sin implementar). El AP de fallback ahora lleva password fija
//     AP_FIXED_PASSWORD ("z21" de momento -- OJO, son menos de los 8
//     caracteres minimos de WPA2, asi que de momento el AP sale abierto en
//     la practica; pendiente alargarla o generarla aleatoriamente y
//     mostrarla en la pantalla del Mega mas adelante). El layout de EEPROM
//     cambio para hacer sitio a las 3 redes, por lo que EEPROM_MAGIC subio
//     de 0x5A a 0x5B (una config vieja de una sola red se ignora limpio en
//     vez de leerse con el offset equivocado, ver comentario en
//     esp8266_wifi.ino).
//   - v0.12 (2026-08-03): ESP_FW_VERSION_MINOR 8->9. Sin constantes nuevas
//     en este header; todos los cambios en esp8266_wifi.ino (y un archivo
//     nuevo, esp8266_wifi/web_assets.h):
//       1. Bug de v0.11: subir el magic global de EEPROM para hacer sitio
//          a las 3 redes WiFi invalido TAMBIEN el usuario/password del
//          portal web sin que hubieran cambiado de formato. Se sustituyo
//          el magic global unico por un byte de validez INDEPENDIENTE por
//          seccion (redes / credenciales del portal / MAC personalizada),
//          para que cambiar el layout de una seccion no afecte a las
//          demas nunca mas.
//       2. El portal web nunca tuvo forma de cambiar su propio usuario/
//          password desde el formulario (solo existian por dentro,
//          siempre con el valor por defecto admin/z21admin) -- ahora
//          /save (seccion "Acceso al portal") permite cambiarlos, con
//          confirmacion de password para evitar quedarse fuera por una
//          errata.
//       3. El HTML/CSS repetido en cada pagina (barra de nav, apertura de
//          <head>) se consolido en pageHead()/pageNav(), que ademas
//          corrige que /log, /sniffer y /test no tenian enlace a Config.
//       4. El CSS compartido se saco a un archivo aparte (web_assets.h),
//          comprimido en gzip y guardado en PROGMEM (flash), servido en
//          /style.css con Content-Encoding: gzip -- no ocupa RAM y pesa
//          menos por la red que repetir 'style=...' en cada <p>. Los
//          colores sueltos inline se sustituyeron por clases (.err/.ok/
//          .warn/.mono/.mega/.esp/.lvl-*) definidas ahi.

#endif // Z21_PROTOCOL_H
