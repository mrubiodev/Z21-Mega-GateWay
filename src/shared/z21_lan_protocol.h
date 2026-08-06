/*
 * z21_lan_protocol.h
 * -------------------
 * Constantes del protocolo LAN Z21 (Z21 LAN Protocol Specification,
 * Modelleisenbahn GmbH, v1.13, 06.11.2023) compartidas EN ESPIRITU entre
 * este proyecto (throttle, cliente) y Z21-Mega-GateWay (servidor/emulador
 * Z21, github.com/mrubiodev/Z21-Mega-GateWay). Mismo criterio que ya usa
 * ese repo entre sus propios dos sketches (ver su AGENT.md): como cliente
 * y servidor son proyectos/repos/toolchains distintos, este archivo NO se
 * enlaza automaticamente entre ambos -- se copia a mano y se mantiene en
 * sincronia igual que ellos ya hacen entre esp8266_wifi/ y mega_z21/.
 *
 * Contenido: union de las constantes que YA usaba cada lado por separado.
 *   - Bloque "handshake / sistema": copiado de Z21-Mega-GateWay
 *     src/shared/z21_protocol.h (el servidor es quien IMPLEMENTA el
 *     handshake que el cliente jamas manda explicitamente salvo
 *     LAN_X_GET_VERSION/STATUS a traves de LAN_X, y LAN_LOGOFF).
 *   - Bloque "X-Bus / traccion / desvios": antes literales inline sin
 *     nombre en este proyecto (Z21Protocol.h), y tambien inline en
 *     Z21-Mega-GateWay/src/mega_z21/mega_z21.ino (p.ej. "x[0] = 0xEF; //
 *     X-Header LAN_X_LOCO_INFO"). Nombrarlos aqui sirve a los DOS lados:
 *     el cliente ya los usa via Z21Protocol.h; el servidor podria
 *     sustituir sus literales inline por estos mismos nombres sin cambiar
 *     ni un valor.
 *
 * Header vs X-Header: los comandos "X-Bus encapsulado" van todos dentro
 * de un paquete Z21 con Header=0x0040; el X-Header (primer byte del
 * payload) es quien de verdad distingue el comando concreto. Un mismo
 * X-Header puede significar cosas distintas segun DB0 (p.ej. 0xE4 es
 * SET_LOCO_DRIVE con DB0=pasos, pero SET_LOCO_FUNCTION con DB0=0xF8) --
 * eso NO cabe en una constante simple, se queda documentado en comentario
 * junto a cada build*()/handle*() de cada lado, como ya hacia cada uno.
 *
 * Si se edita este archivo, replicar el cambio en la copia del otro repo
 * (no hay mecanismo de include compartido entre Arduino/PlatformIO y
 * distintos repos de GitHub).
 */
#ifndef Z21_LAN_PROTOCOL_H
#define Z21_LAN_PROTOCOL_H

// -----------------------------------------------------------------------
// Puerto UDP estandar del protocolo LAN Z21
// -----------------------------------------------------------------------
#define Z21_UDP_PORT 21105

// -----------------------------------------------------------------------
// Headers Z21 (van en el campo Header del paquete, offset 2-3, uint16 LE)
// -----------------------------------------------------------------------
#define LAN_GET_SERIAL_NUMBER 0x10
#define LAN_GET_COMMUNICATION_INFO 0x12 // (Z21-Mega-GateWay, no usado aun por el cliente)
#define LAN_GET_CODE 0x18
#define LAN_GET_HWINFO 0x1A
#define LAN_LOGOFF 0x30
#define LAN_X 0x40 // canal X-Bus encapsulado: el X-Header real va en el payload, ver mas abajo
#define LAN_SET_BROADCASTFLAGS 0x50
#define LAN_GET_BROADCASTFLAGS 0x51
#define LAN_SYSTEMSTATE_DATACHANGED 0x84
#define LAN_SYSTEMSTATE_GETDATA 0x85
#define LAN_CAN_DETECTOR 0xC4 // (Z21-Mega-GateWay, no usado aun por el cliente)

// -----------------------------------------------------------------------
// X-Headers (primer byte del payload cuando Header == LAN_X == 0x40). Un
// mismo valor de X-Header puede representar VARIOS comandos segun DB0
// (segundo byte del payload) -- ver comentario de cabecera de este
// archivo. OJO al comparar con Z21-Mega-GateWay/src/shared/z21_protocol.h:
// ahi "LAN_X_GET_VERSION"/"LAN_X_GET_STATUS" valen 0x40 -- ese 0x40 es el
// Header EXTERNO (LAN_X, "esto va por el canal X-Bus"), NO el X-Header
// real que distingue el comando. Aqui se separan ambos conceptos con
// nombres distintos (LAN_X vs X_HEADER_*) para que no se confundan.
// -----------------------------------------------------------------------
#define X_HEADER_GET_VERSION 0x21     // DB0=0x21 (coincide con el propio X-Header, es asi en el PDF)
#define X_HEADER_SET_TRACK_POWER 0x21 // DB0=0x80 (OFF) / 0x81 (ON)
#define X_HEADER_GET_STATUS 0x21      // DB0=0x24
#define X_HEADER_SET_STOP 0x80        // parada de emergencia global (X-Bus), sin DB0 (payload de 1 byte)
#define X_HEADER_SET_TURNOUT 0x53
#define X_HEADER_GET_LOCO_INFO 0xE3   // DB0=0xF0
#define X_HEADER_SET_LOCO_DRIVE 0xE4  // DB0=pasos (0x10=14, 0x12=28, 0x13=128)
#define X_HEADER_SET_LOCO_FUNCTION 0xE4 // DB0=0xF8 (mismo X-Header que SET_LOCO_DRIVE, distinto DB0)
#define X_HEADER_LOCO_INFO 0xEF       // reply/broadcast a GET_LOCO_INFO

// -----------------------------------------------------------------------
// HwType (LAN_GET_HWINFO) -- "black Z21", variante 2013, la mas compatible
// con la app oficial. Evitar 0x00000204 (z21 start): activa restricciones
// de esa version. Solo lo usa el lado SERVIDOR (quien responde HWINFO).
// -----------------------------------------------------------------------
#define D_HWT_Z21_NEW 0x00000201UL

// LAN_GET_CODE -- nivel de bloqueo de funciones (solo servidor)
#define Z21_NO_LOCK 0x00

// -----------------------------------------------------------------------
// Bitmask de CentralState / CentralStateEx dentro de SystemState (ver
// LAN_SYSTEMSTATE_DATACHANGED). El cliente los necesita para INTERPRETAR
// el estado de la via si algun dia procesa LAN_SYSTEMSTATE_DATACHANGED
// (ver docs/04-plan-de-desarrollo.md, pendiente); el servidor los usa
// para CONSTRUIRLO.
// -----------------------------------------------------------------------
#define CS_EMERGENCY_STOP 0x01
#define CS_TRACK_VOLTAGE_OFF 0x02
#define CS_SHORT_CIRCUIT 0x04
#define CS_PROGRAMMING_MODE_ACTIVE 0x20

// SystemState.Capabilities (PDF oficial seccion 2.18, byte 15). Solo lo
// usa el lado SERVIDOR (quien declara sus capacidades).
#define CAP_DCC 0x01
#define CAP_LOCO_CMDS 0x10

// -----------------------------------------------------------------------
// Historial de cambios de este header compartido (misma convencion que
// Z21-Mega-GateWay/src/shared/z21_protocol.h: recordar replicar cualquier
// edicion en la copia del otro repo, y anadir aqui una linea con que
// cambio y por que).
// -----------------------------------------------------------------------
//   - v1.0 (2026-08-06): creacion inicial en el lado cliente, union de
//     las constantes de Z21-Mega-GateWay/src/shared/z21_protocol.h
//     (bloque handshake/sistema) y las que ya usaba
//     infrastructure/network/Z21Protocol.h como literales inline sin
//     nombre (bloque X-Bus/traccion/desvios, ahora con nombre propio
//     X_HEADER_*). Aclarado ademas que LAN_X_GET_VERSION/LAN_X_GET_STATUS
//     en la copia del servidor valen 0x40 (el Header externo LAN_X), no
//     el X-Header real (0x21) -- aqui se separan como LAN_X vs
//     X_HEADER_GET_VERSION/X_HEADER_GET_STATUS para no confundirlos.

#endif // Z21_LAN_PROTOCOL_H
