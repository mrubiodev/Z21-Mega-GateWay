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

#define LAN_GET_COMMUNICATION_INFO 0x12

#define LAN_CAN_DETECTOR 0xC4

// Accesorios (agujas/señales de 2 aspectos/desacopladores/descarriladores
// biestables) — PDF oficial sección 5 "Switching". Mismo XHeader 0x43
// para la petición (5.1) y la respuesta (5.3): se distinguen por
// DataLen/DB2, no por el XHeader.
#define LAN_X_GET_TURNOUT_INFO 0x43
#define LAN_X_TURNOUT_INFO 0x43
#define LAN_X_SET_TURNOUT 0x53

// Accesorios EXTENDIDOS (señales de más de 2 aspectos, decodificadores
// DCCext según RCN-213) — PDF oficial secciones 5.4-5.6. Direccionamiento
// DISTINTO al de LAN_X_(GET_)TURNOUT(_INFO) de arriba: aquí la dirección
// es la RawAddress de RCN-213 tal cual (primer decodificador extendido =
// RawAddress 4, mostrado como "dirección 1" en las UIs), sin la
// conversión a puerto/salida que sí aplica la sección 5 a los turnouts
// normales — ver ExtAccessoryState en traction_types.h. Igual que pasa
// con 0x43 arriba, el XHeader 0x44 se reutiliza para la petición (5.5) y
// la respuesta (5.6): se distinguen por DataLen, no por el XHeader.
#define LAN_X_SET_EXT_ACCESSORY 0x54
#define LAN_X_GET_EXT_ACCESSORY_INFO 0x44
#define LAN_X_EXT_ACCESSORY_INFO 0x44

// Byte "Status" de LAN_X_EXT_ACCESSORY_INFO (PDF sección 5.6, DB3)
#define EXT_ACCESSORY_STATUS_VALID 0x00
#define EXT_ACCESSORY_STATUS_UNKNOWN 0xFF

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
// declarar con honestidad; el resto (MM, RailCom, detectores) NO se marca
// porque no hay backend real para ellos todavía.
// CAP_ACCESSORY_CMDS añadido junto con el soporte de LAN_X_(GET/SET)_
// TURNOUT (agujas/señales de 2 aspectos) y LAN_X_(GET/SET)_EXT_ACCESSORY
// (señales de más de 2 aspectos) — ambos aceptan y responden comandos LAN
// de accesorios de forma coherente aunque, con backend XpressNet, los
// extendidos todavía no lleguen de verdad al bus físico (ver limitación
// documentada en traction_backend_xpressnet.h). El bit describe lo que
// acepta el LAN de esta central, no si hay hardware detrás confirmando
// cada comando — igual que CAP_LOCO_CMDS tampoco implica que haya una vía
// con corriente real conectada. El ESP no interpreta este byte (solo lo
// transporta dentro del datagrama Z21), pero la constante vive aquí para
// que las 3 copias del header describan el mismo protocolo completo.
#define CAP_DCC 0x01
#define CAP_LOCO_CMDS 0x10
#define CAP_ACCESSORY_CMDS 0x20

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
// Client-id en el payload de FRAME_TYPE_Z21 (v0.20, soporte multi-cliente
// real) -- NO cambia la capa de sync/checksum de arriba.
// -----------------------------------------------------------------------
// Desde v0.20 el payload de un frame FRAME_TYPE_Z21 deja de ser el
// datagrama Z21 pelado y pasa a ser:
//
//   [CLIENT_ID(1)][datagrama Z21 tal cual iba/venía por UDP...]
//
// Sentido ESP->Mega: CLIENT_ID = índice de cliente en la tabla del ESP
// (z21Clients[] / trackClient(), ver esp8266_wifi.ino) que mandó esa
// petición UDP concreta.
// Sentido Mega->ESP: CLIENT_ID = a qué cliente (mismo índice) va dirigido
// este frame; el ESP simplemente lo reenvía por UDP a la IP/puerto de ese
// índice — sigue sin interpretar el contenido Z21 ("el ESP solo
// transporta", ver AGENT.md), el client-id es lo único que mira.
// Esto SOLO afecta a FRAME_TYPE_Z21: HELLO/NET_INFO/SYNC_ACK/HEARTBEAT no
// llevan cliente y no cambian de formato.
//
// MOTIVO: hasta v0.18/v0.19 el ESP adivinaba a qué cliente pertenecía
// cada respuesta con una cola FIFO (heurística "el Mega responde en el
// mismo orden en que pregunta", ver esos changelogs más abajo) y
// CUALQUIER frame que el Mega mandase que no fuera respuesta directa a la
// última petición (broadcasts reales: LAN_X_BC_STOPPED,
// LAN_X_BC_TRACK_POWER_OFF/ON, LAN_X_TURNOUT_INFO,
// LAN_X_EXT_ACCESSORY_INFO, LAN_X_LOCO_INFO tras un cambio) se enviaba
// solo al primero de la cola, nunca a los demás clientes conectados. Con
// client-id explícito, el Mega decide con precisión a quién responde de
// forma directa y a quién notificar como broadcast (ver
// clientBroadcastFlags[] en mega_z21.ino, que sustituye al antiguo valor
// global único de broadcast flags).
//
// Los bits de LAN_SET_BROADCASTFLAGS / LAN_GET_BROADCASTFLAGS (PDF
// oficial, sección 2.16) que el Mega ya sabe disparar de verdad de forma
// selectiva por cliente. El resto de bits del protocolo real (RBus,
// RailCom, CAN, LocoNet...) siguen sin implementar y se ignoran igual que
// antes. NOTA: igual que otras asunciones de este repo, los valores de
// estos bits están tomados de memoria del PDF oficial y no se han vuelto
// a contrastar línea a línea contra el documento en este cambio.
#define Z21_MAX_CLIENTS 16 // DEBE coincidir con MAX_TRACKED_CLIENTS (esp8266_wifi.ino, este sketch)
#define CLIENT_ID_NONE 0xFF // "sin cliente concreto" (ver mega_z21.ino) — nunca es un slot válido real
#define BCFLAG_BASIC 0x00000001UL       // LAN_X_BC_STOPPED, LAN_X_BC_TRACK_POWER_OFF/ON,
                                         // LAN_X_TURNOUT_INFO, LAN_X_EXT_ACCESSORY_INFO,
                                         // LAN_X_LOCO_INFO (tras un cambio, a clientes
                                         // distintos del que mandó el comando)
#define BCFLAG_SYSTEMSTATE 0x00000100UL // LAN_SYSTEMSTATE_DATACHANGED como broadcast
                                         // (definido para cuando se dispare como broadcast
                                         // de verdad; hoy solo se usa como respuesta directa)
//
// LIMITACIÓN CONOCIDA (no resuelta a propósito): si este ESP recicla un
// slot de z21Clients[] por LRU (tabla llena) para un cliente físico
// distinto, el Mega no se entera de ese reciclaje y seguiría aplicando
// clientBroadcastFlags[slot] del cliente anterior hasta que el nuevo
// ocupante mande su propio LAN_SET_BROADCASTFLAGS. Con
// MAX_TRACKED_CLIENTS=16 es un caso raro en la práctica. La solución
// completa (un frame FRAME_TYPE_CLIENT_RESET(clientId) que este ESP mande
// al reasignar un slot) queda como siguiente paso, no implementada para
// no ampliar el framing más de lo estrictamente necesario para este
// cambio.

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
#define MEGA_FW_VERSION_MINOR 12
#define ESP_FW_VERSION_MAJOR 0
#define ESP_FW_VERSION_MINOR 17

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
//   - v0.13 (2026-08-03): ESP_FW_VERSION_MINOR 9->10. Sin constantes
//     nuevas en este header; cambios en esp8266_wifi.ino y web_assets.h:
//     el formulario de configuracion ahora puede escanear las redes WiFi
//     visibles (boton "Buscar redes WiFi" -> GET /scan, que devuelve JSON
//     con SSID/RSSI/cifrado) y rellenar cualquiera de los 3 huecos de red
//     con un clic, en vez de tener que escribir el SSID a mano. La logica
//     vive en un app.js nuevo (mismo tratamiento gzip+PROGMEM que ya tenia
//     el CSS, servido en /app.js). Si el ESP esta en modo AP fallback,
//     /scan activa la interfaz STA solo durante el escaneo y vuelve al
//     modo anterior al terminar, para no interrumpir a los clientes ya
//     conectados al AP mas de lo estrictamente necesario.
//   - v0.14 (2026-08-04): ESP_FW_VERSION_MINOR 10->11. Sin constantes
//     nuevas en este header; cambios solo en esp8266_wifi.ino: backup y
//     restore de la configuracion (redes WiFi, credenciales del portal,
//     MAC personalizada) desde el propio portal. GET /backup descarga la
//     config actual como JSON (formatVersion:1); POST /restore acepta ese
//     mismo JSON subido como archivo y lo aplica (con validacion basica:
//     requiere formatVersion:1 y al menos un SSID, o se ignora entero) y
//     reinicia. El JSON se parsea a mano (busqueda de "clave":"valor"
//     literal, sin libreria) porque es un formato plano generado y leido
//     por el mismo firmware -- mismo criterio que ya se uso en /scan. El
//     archivo de backup lleva las passwords EN CLARO (es la unica forma
//     de que el restore no requiera volver a teclearlas); el portal avisa
//     de esto junto al enlace de descarga.
//   - v0.15 (2026-08-04): ESP_FW_VERSION_MINOR 11->12. Sin constantes
//     nuevas en este header; dos arreglos en esp8266_wifi.ino:
//       1. htmlEscape(): varios valores que se reinsertan en las paginas
//          del portal (SSID guardado o escaneado, usuario del portal,
//          texto del log) no los escribe necesariamente el dueño del ESP
//          -- un SSID ajeno con una comilla rompia el value='...' del
//          formulario, y uno con <script> se habria ejecutado sin mas al
//          ver el propio /log o / (self-XSS, pero real desde que existe
//          el escaneo de redes en /scan). Se añadio htmlEscape() y se
//          aplico en todos esos puntos.
//       2. AP_FIXED_PASSWORD paso de "z21" (3 caracteres, por debajo del
//          minimo de WPA2 -- el AP salia abierto en la practica) a
//          "z21admin" (8 caracteres, reutilizando el mismo valor por
//          defecto que ya existia para cfgWebPass en vez de inventar un
//          secreto nuevo). Sigue siendo temporal/fijo, pero ahora cifra
//          de verdad.
//   - v0.16 (2026-08-04): ESP_FW_VERSION_MINOR 12->13. Sin constantes
//     nuevas en este header; cambio solo en esp8266_wifi.ino: soporte OTA
//     (ArduinoOTA) para poder actualizar el firmware por WiFi desde el
//     IDE de Arduino, sin cable USB. El ESP aparece como "puerto de red"
//     con hostname z21emulator-<chipid> (visible tambien en el estado del
//     portal, /). Protegido con la misma password que el portal web
//     (cfgWebPass) en vez de una credencial nueva -- si se cambia desde
//     /save, se relee en el siguiente arranque (que ya ocurre solo tras
//     guardar). setupOTA() se llama una vez en setup() tras levantar
//     WiFi y el portal; ArduinoOTA.handle() se llama en cada loop().
//   - v0.17 (2026-08-04): ESP_FW_VERSION_MINOR 13->14. Sin constantes
//     nuevas en este header; dos cambios en esp8266_wifi.ino/web_assets.h:
//       1. Password de OTA SEPARADA de la del portal (cfgOtaPass, nueva
//          seccion de EEPROM propia -- EEPROM_ADDR_OTAPASS_VALID). El
//          v0.16 original reutilizaba cfgWebPass para OTA; con dos
//          privilegios tan distintos (ver/editar config vs. flashear
//          firmware arbitrario) conviene que compartan password solo si
//          se elige explicitamente, no por defecto. Editable desde /save
//          igual que la del portal (doble campo para evitar typos),
//          incluida en el backup/restore (campo "otaPass").
//       2. Rediseño visual del portal: paleta de color (azul acero),
//          cabecera y barra de nav con fondo solido, y cada bloque de
//          contenido de las 4 paginas metido en un <div class='card'>
//          (fondo blanco, borde suave, sombra ligera) en vez de texto
//          plano corrido -- pageHead()/pageFoot() ahora abren y cierran
//          tambien un contenedor .wrap centrado. Todo sigue viviendo en
//          el mismo CSS comprimido en gzip de siempre (ver web_assets.h).
//   - v0.18 (2026-08-05): ESP_FW_VERSION_MINOR 14->15. Sin constantes
//     nuevas en este header; cambios en esp8266_wifi.ino/web_assets.h:
//       1. REVISION de soporte multi-cliente. Hasta ahora una sola
//          variable global (lastClientIP/lastClientPort) se sobreescribia
//          con cada UDP entrante -- con un cliente Z21 conectado no se
//          nota, pero con dos o mas a la vez la respuesta del Mega podia
//          mandarse al cliente equivocado. Se sustituyo por una cola FIFO
//          (replyQueue, CLIENT_REPLY_QUEUE_LEN=8): cada peticion
//          reenviada al Mega empuja el cliente a la cola, cada respuesta
//          Z21 del Mega hace pop del primero -- mejora real sobre "el
//          ultimo que hablo", pero sigue siendo una heuristica: el
//          framing ESP<->Mega no lleva identificador de cliente, y los
//          mensajes de tipo BROADCAST del protocolo real (cambio de
//          estado de una loco, alimentacion de via) deberian mandarse a
//          TODOS los clientes con el flag correspondiente activo (ver
//          LAN_SET_BROADCASTFLAGS), no solo al de la cabeza de la cola --
//          eso NO esta implementado todavia, documentado como limitacion
//          conocida (ver comentario grande junto a la cola en el .ino).
//          Una solucion completa requeriria tocar el Mega para etiquetar
//          sus respuestas con un indice de cliente.
//       2. Panel /clients nuevo: lista los clientes Z21 vistos (IP, MAC
//          si se puede resolver, ultima vez visto) y permite ponerles un
//          nombre "amigable", persistido en EEPROM (nueva seccion,
//          EEPROM_SIZE subio de 512 a 1024 para hacer sitio). Identidad
//          por MAC cuando es posible (mismo dispositivo aunque le cambie
//          la IP), con fallback a IP -- la MAC SOLO se puede resolver
//          cuando el ESP esta en modo AP (via wifi_softap_get_station_info()
//          del SDK); en modo STA (conectado al router de casa, el caso
//          habitual) no hay API estandar para obtener la MAC de otro
//          dispositivo de la red, asi que ese cliente se sigue solo por
//          IP y se trata como "nuevo" si esa IP cambia (limitacion de
//          plataforma). Los clientes con nombre puesto nunca se
//          desalojan de la tabla (maximo MAX_TRACKED_CLIENTS=8), asi que
//          siguen apareciendo como "Desconectado" en vez de desaparecer.
//   - v0.19 (2026-08-05): ESP_FW_VERSION_MINOR 15->16. Sin constantes
//     nuevas en este header; cambios solo en esp8266_wifi.ino, tras
//     aclarar que MAX_TRACKED_CLIENTS/CLIENT_REPLY_QUEUE_LEN eran numeros
//     arbitrarios (no limites de hardware/RAM):
//       - MAX_TRACKED_CLIENTS: 8 -> 16 (registro de clientes /clients).
//       - CLIENT_REPLY_QUEUE_LEN: 8 -> 16 (peticiones en vuelo hacia el Mega).
//       - EEPROM_SIZE: 1024 -> 2048 para hacer sitio de sobra (mismo
//         sector de flash de siempre, sin coste real).
//       - WiFi.softAP() ahora fija max_connection=8 explicitamente -- ESE
//         SI es un limite real de hardware/SDK del ESP8266 en modo AP (el
//         core lo deja en 4 por defecto si no se especifica). Solo
//         importa si el ESP hace de AP el mismo (fallback); en modo STA
//         (conectado al router de casa) no aplica -- lo gestiona el router.
//     El cuello de botella real para muchos clientes activos a la vez
//     sigue siendo el enlace serie unico hacia el Mega, no ninguno de
//     estos numeros -- documentado junto a CLIENT_REPLY_QUEUE_LEN.
//   - v0.20 (2026-08-05): ESP_FW_VERSION_MINOR 16->17, MEGA_FW_VERSION_MINOR
//     11->12 (corregido de paso: esta copia se habia quedado con un 8
//     desactualizado). CAMBIO DE FORMATO DEL PAYLOAD DE FRAME_TYPE_Z21 (no
//     de la capa de framing [SYNC][TYPE][LEN][...][CHK], que no se toca):
//     ahora lleva 1 byte de CLIENT_ID delante del datagrama Z21 real.
//     Nuevas constantes Z21_MAX_CLIENTS, CLIENT_ID_NONE, BCFLAG_BASIC,
//     BCFLAG_SYSTEMSTATE (ver comentario grande junto a ellas, mas
//     arriba). ESTO SUSTITUYE la cola FIFO (replyQueue/PendingReply,
//     CLIENT_REPLY_QUEUE_LEN) descrita en v0.18/v0.19 arriba -- ya no
//     hace falta y se ha quitado del .ino: en vez de adivinar el
//     destinatario por orden de llegada, cada UDP entrante se etiqueta
//     con su slot real de z21Clients[] (trackClient() ahora devuelve el
//     slot) antes de mandarlo al Mega, y cada frame que vuelve del Mega
//     trae ese mismo slot para saber exactamente a que IP/puerto
//     reenviarlo -- ya no hay heuristica de por medio, y el Mega puede
//     mandar tantos frames como clientes distintos necesite notificar
//     (broadcasts reales: LAN_X_BC_STOPPED, LAN_X_TURNOUT_INFO, etc, ver
//     mega_z21.ino). ACTUALIZAR LOS DOS SKETCHES A LA VEZ: un ESP v0.20
//     hablando con un Mega anterior a v0.14 (o viceversa) desincroniza la
//     lectura de cada FRAME_TYPE_Z21 en un byte. No afecta a HELLO/
//     NET_INFO/SYNC_ACK/HEARTBEAT.
//     LIMITACION CONOCIDA (no resuelta a proposito): si trackClient()
//     reasigna un slot por LRU (tabla llena) para un cliente fisico
//     distinto justo mientras ese slot tenia una peticion en vuelo hacia
//     el Mega, la respuesta llegaria dirigida al cliente NUEVO que ocupa
//     ahora el slot. Raro con MAX_TRACKED_CLIENTS=16 en la practica; la
//     solucion completa (frame FRAME_TYPE_CLIENT_RESET) queda como
//     siguiente paso, no implementada para no ampliar el framing mas de
//     lo necesario para este cambio.
//     NOTA IMPORTANTE (por como se descubrio el bug de compilacion que
//     motivo este apunte): la copia de este header en src/shared/ se
//     habia quedado desactualizada respecto a esta copia real (le
//     faltaban varias entradas de historial y estas constantes de
//     multi-red WiFi/OTA/etc ya llevaban tiempo solo aqui) -- si se
//     vuelve a usar src/shared/ como plantilla para regenerar las copias
//     de los sketches, verificar primero con un diff contra las copias
//     reales que compilan, no al reves.

//   - v0.21 (2026-08-06): Sin cambio de MEGA_FW_VERSION_MINOR/ESP_FW_VERSION_MINOR
//     (no hay cambio de comportamiento, solo de constantes documentadas).
//     Esta copia (src/esp8266_wifi/) se habia quedado desincronizada de
//     src/shared/ y src/mega_z21/protocol/ (que si coincidian entre si):
//     le faltaban LAN_GET_COMMUNICATION_INFO, LAN_CAN_DETECTOR, el bloque
//     completo de accesorios (LAN_X_(GET_)TURNOUT_INFO/SET_TURNOUT,
//     LAN_X_SET_EXT_ACCESSORY/GET_EXT_ACCESSORY_INFO/EXT_ACCESSORY_INFO,
//     EXT_ACCESSORY_STATUS_*) y CAP_ACCESSORY_CMDS -- introducidas en el
//     lado Mega en v0.11/v0.12 (ver arriba) y nunca replicadas aqui. No
//     rompia la compilacion ni el comportamiento porque este sketch no
//     interpreta el payload Z21 ("el ESP solo transporta", ver AGENT.md;
//     su sniffer decodifica por XHeader crudo, sin necesitar una entrada
//     por comando) -- pero mantenia esta copia incompleta como referencia
//     y contradecia la politica de las "3 copias identicas" que este mismo
//     historial pide seguir. Corregido anadiendo el bloque tal cual esta
//     en src/shared/. Detectado durante una revision cruzada con el
//     proyecto cliente (z21-throttle, ver su docs/05-convenciones-
//     compartidas-con-servidor.md), no por un fallo de compilacion como
//     las dos veces anteriores que este historial documenta (v0.14/v0.20)
//     -- motivo de mas para automatizar esta comprobacion en vez de
//     confiar solo en acordarse.

#endif // Z21_PROTOCOL_H
