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
// con corriente real conectada.
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
// Client-id en el payload de FRAME_TYPE_Z21 (v0.14, soporte multi-cliente
// real) -- NO cambia la capa de sync/checksum de arriba.
// -----------------------------------------------------------------------
// Desde v0.14 el payload de un frame FRAME_TYPE_Z21 deja de ser el
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
// MOTIVO: antes de v0.14/v0.20 (mega/esp) el ESP adivinaba a qué cliente
// pertenecía cada respuesta con una cola FIFO (heurística "el Mega
// responde en el mismo orden en que pregunta", ver v0.18/v0.19 del
// changelog del ESP más abajo) y CUALQUIER frame que el Mega mandase que
// no fuera respuesta directa a la última petición (broadcasts reales:
// LAN_X_BC_STOPPED, LAN_X_BC_TRACK_POWER_OFF/ON, LAN_X_TURNOUT_INFO,
// LAN_X_EXT_ACCESSORY_INFO, LAN_X_LOCO_INFO tras un cambio) se enviaba
// solo al primero de la cola, nunca a los demás clientes conectados. Con
// client-id explícito, el Mega decide con precisión a quién responde de
// forma directa y a quién notificar como broadcast (ver
// clientBroadcastFlags[] en mega_z21.ino, que sustituye al antiguo valor
// global único de broadcast flags).
//
// Los bits de LAN_SET_BROADCASTFLAGS / LAN_GET_BROADCASTFLAGS (PDF
// oficial, sección 2.16) que este firmware ya sabe disparar de verdad de
// forma selectiva por cliente. El resto de bits del protocolo real (RBus,
// RailCom, CAN, LocoNet...) siguen sin implementar y se ignoran igual que
// antes. NOTA: igual que otras asunciones de este repo (ver "ASUNCIONES A
// VALIDAR" en traction_backend_xpressnet.h), los valores de estos bits
// están tomados de memoria del PDF oficial y no se han vuelto a
// contrastar línea a línea contra el documento en este cambio.
#define Z21_MAX_CLIENTS 16 // DEBE coincidir con MAX_TRACKED_CLIENTS del ESP (esp8266_wifi.ino)
#define CLIENT_ID_NONE 0xFF // "sin cliente concreto": usado cuando un evento Z21 no es
                             // respuesta a ninguna petición de red (p.ej. el botón físico
                             // de e-stop, ver mega_z21.ino) — nunca es un slot válido real
#define BCFLAG_BASIC 0x00000001UL       // LAN_X_BC_STOPPED, LAN_X_BC_TRACK_POWER_OFF/ON,
                                         // LAN_X_TURNOUT_INFO, LAN_X_EXT_ACCESSORY_INFO,
                                         // LAN_X_LOCO_INFO (tras un cambio, a clientes
                                         // distintos del que mandó el comando)
#define BCFLAG_SYSTEMSTATE 0x00000100UL // LAN_SYSTEMSTATE_DATACHANGED como broadcast
                                         // (definido para cuando se dispare como broadcast
                                         // de verdad; hoy solo se usa como respuesta directa)
//
// LIMITACIÓN CONOCIDA (no resuelta a propósito): el ESP puede reciclar un
// slot de z21Clients[] por LRU (tabla llena) para un cliente físico
// distinto; el Mega no se entera de ese reciclaje y seguiría aplicando
// clientBroadcastFlags[slot] del cliente anterior hasta que el nuevo
// ocupante mande su propio LAN_SET_BROADCASTFLAGS. Con Z21_MAX_CLIENTS=16
// es un caso raro en la práctica. La solución completa (un frame
// FRAME_TYPE_CLIENT_RESET(clientId) que el ESP mande al reasignar un
// slot) queda como siguiente paso, no implementada para no ampliar el
// framing más de lo estrictamente necesario para este cambio.

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
//     en este header; cambios solo en esp8266_wifi.ino (log con niveles,
//     sniffer bidireccional con decodificación de comandos Z21, MAC de
//     red configurable). No afecta a mega_z21.ino.
//   - v0.8 (2026-08-03): ESP_FW_VERSION_MINOR 5->6. Sin constantes nuevas
//     en este header; cambio solo en esp8266_wifi.ino (MAC por defecto con
//     prefijo 84:2B:BC en vez de la de fábrica del chip). No afecta a
//     mega_z21.ino.
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
//   - v0.11 (2026-08-04): MEGA_FW_VERSION_MINOR 8->9. Nuevas constantes
//     LAN_X_GET_TURNOUT_INFO/LAN_X_TURNOUT_INFO/LAN_X_SET_TURNOUT (PDF
//     seccion 5, "Switching") -- primer soporte de accesorios (agujas,
//     senales de 2 aspectos, desacopladores, descarriladores biestables:
//     todos son el mismo decodificador de accesorios DCC de 2 salidas a
//     nivel de protocolo Z21). Nuevo AccessoryState en traction_types.h,
//     nuevos metodos setTurnout/requestTurnoutRefresh/getTurnoutState en
//     ITractionBackend, implementados en ambos backends (Dummy y
//     XpressNet). Senales de mas de 2 aspectos (LAN_X_SET_EXT_ACCESSORY,
//     PDF seccion 5.4) quedan fuera todavia. No afecta a esp8266_wifi.ino
//     (el ESP sigue sin interpretar el contenido Z21).
//   - v0.12 (2026-08-04): MEGA_FW_VERSION_MINOR 9->10. Nuevas constantes
//     LAN_X_SET_EXT_ACCESSORY/LAN_X_GET_EXT_ACCESSORY_INFO/LAN_X_EXT_
//     ACCESSORY_INFO y EXT_ACCESSORY_STATUS_* (PDF secciones 5.4-5.6,
//     RCN-213) -- soporte de senales de mas de 2 aspectos, ultimo hueco
//     del capitulo 5 "Switching" del protocolo. Nuevo ExtAccessoryState
//     en traction_types.h (tabla separada de AccessoryState: direcciones
//     RawAddress de RCN-213, espacio distinto al FAdr de los turnouts
//     normales) y nuevo ExtAccessoryStateStore. Nuevos metodos
//     setExtAccessory/requestExtAccessoryRefresh/getExtAccessoryState en
//     ITractionBackend, implementados en Dummy (estado en RAM, siempre al
//     dia) y en XpressNet (TAMBIEN solo estado en RAM en esta v1: la
//     libreria Digital-MoBa/XpressNet es anterior a RCN-213/DCCext y no
//     tiene forma de mandar este paquete por el bus fisico -- ver
//     limitacion documentada explicitamente en traction_backend_
//     xpressnet.h, punto 5 de "ASUNCIONES A VALIDAR"). Tambien se anade
//     CAP_ACCESSORY_CMDS a SystemState.Capabilities (LAN_
//     SYSTEMSTATE_DATACHANGED) -- ya se aceptaban comandos de turnout
//     desde v0.11 y no se habia marcado el bit correspondiente. No afecta
//     a esp8266_wifi.ino.
//   - v0.13 (2026-08-04): MEGA_FW_VERSION_MINOR 10->11. Cierra el capitulo
//     4 "Driving" de la lista v1 del spec: LAN_X_SET_LOCO_E_STOP (0x92,
//     PDF 4.5) y LAN_X_PURGE_LOCO (XHeader 0xE3 compartido con LAN_X_
//     GET_LOCO_INFO, distinguido por DB0=0x44 vs 0xF0, PDF 4.6). Nuevo
//     metodo purgeLoco() en ITractionBackend y release() en
//     LocoStateStore (liberar el slot de una direccion). Sin constantes
//     nuevas en este header: los XHeader/DB0 involucrados (0x92, 0xF0,
//     0x44 dentro de 0xE3) siguen el estilo ya establecido del proyecto
//     de literal+comentario en el dispatcher de mega_z21.ino en vez de
//     macro, igual que 0x21/0xE3/0xF1/0xE4/0x43/0x53 de mas arriba -- ver
//     mega_z21.ino para el detalle. No afecta a esp8266_wifi.ino (el
//     sniffer ya decodifica LAN_X de forma generica por XHeader, sin
//     necesitar una entrada por comando).
//   - v0.14 (2026-08-05): MEGA_FW_VERSION_MINOR 11->12, ESP_FW_VERSION_MINOR
//     16->17. CAMBIO DE FORMATO DEL PAYLOAD DE FRAME_TYPE_Z21 (no de la
//     capa de framing [SYNC][TYPE][LEN][...][CHK], que no se toca): ahora
//     lleva 1 byte de CLIENT_ID delante del datagrama Z21 real. Nuevas
//     constantes Z21_MAX_CLIENTS, CLIENT_ID_NONE, BCFLAG_BASIC,
//     BCFLAG_SYSTEMSTATE (ver comentario grande junto a ellas, mas arriba).
//     Sustituye la cola FIFO heuristica que tenia el ESP por
//     identificacion explicita de cliente, y permite que el Mega distinga
//     respuesta directa de broadcast a clientes suscritos -- antes
//     LAN_X_BC_STOPPED, LAN_X_BC_TRACK_POWER_OFF/ON, LAN_X_TURNOUT_INFO,
//     LAN_X_EXT_ACCESSORY_INFO y LAN_X_LOCO_INFO (tras un cambio) se
//     trataban todos como respuesta directa a un solo cliente, con lo que
//     un segundo cliente Z21 conectado a la vez (app movil + Rocrail/
//     JMRI, por ejemplo) dejaba de enterarse de los cambios que hiciera
//     el otro. clientBroadcastFlags[] (array por cliente, mega_z21.ino)
//     sustituye al antiguo valor global unico de broadcast flags.
//     ACTUALIZAR LOS DOS SKETCHES A LA VEZ: un Mega v0.14 hablando con un
//     ESP anterior a v0.20 (o viceversa) desincroniza la lectura de cada
//     FRAME_TYPE_Z21 en un byte. No afecta a HELLO/NET_INFO/SYNC_ACK/
//     HEARTBEAT. NOTA IMPORTANTE (por como se descubrio este bug): la
//     copia de este header en src/shared/ se habia quedado desactualizada
//     respecto a esta copia real (le faltaban LAN_CAN_DETECTOR,
//     LAN_GET_COMMUNICATION_INFO, todas las constantes de accesorios/
//     turnouts/ext-accessory y CAP_ACCESSORY_CMDS) -- si se vuelve a usar
//     src/shared/ como plantilla para regenerar las copias de los
//     sketches, verificar primero con un diff contra las copias reales
//     que compilan, no al reves.

#endif // Z21_PROTOCOL_H
