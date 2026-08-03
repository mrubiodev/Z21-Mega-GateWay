/*
 * mega_z21.ino
 * ------------
 * Sketch para el ATmega2560. Esta primera versión se centra EN EXCLUSIVA
 * en la prioridad nº1 del proyecto: que la app Z21 oficial reconozca el
 * dispositivo como una central legítima (ver docs/Z21_EMULATOR_SPEC.md
 * sección 5, y AGENT.md).
 *
 * HANDSHAKE DE IDENTIFICACIÓN (prioridad nº1, ver AGENT.md): LAN_GET_
 * SERIAL_NUMBER, LAN_GET_HWINFO, LAN_X_GET_VERSION/STATUS, LAN_GET_CODE
 * siguen siendo simulados/fijos a propósito (lo único que le importa a
 * la app Z21 es que respondan de forma coherente, no que reflejen
 * hardware real) — no tocar sin releer la sección 5 del spec.
 *
 * TRACCIÓN Y LOCOMOTORAS: YA NO son dummy puro. El núcleo Z21 de este
 * sketch traduce datagramas LAN_X <-> llamadas a `traction`
 * (ITractionBackend, ver traction_backend.h) — de qué backend concreto
 * esté detrás (DummyTractionBackend sin hardware, o
 * XpressNetTractionBackend como esclavo real de la MultiMaus) se decide
 * en una sola línea al principio de este fichero (TRACTION_BACKEND_
 * SELECTED) y es invisible para el resto del sketch. Esto es la "capa de
 * abstracción backend de tracción" que ya prometía AGENT.md — ver ese
 * fichero y docs/Z21_EMULATOR_SPEC.md sección 11 para el roadmap
 * (XpressNet v1 -> LocoNet v2 -> DCC directo v3, mismo core Z21 sin
 * reescribir nada de este fichero).
 *
 * DIAGNÓSTICO: el Mega manda un heartbeat periódico al ESP con tiempos de
 * ciclo, RAM libre y contadores de frames — el ESP lo usa para saber si el
 * Mega está vivo y mostrarlo en la web (ver AGENT.md, sección Diagnóstico).
 * Además hay un watchdog hardware: si loop() se cuelga, el Mega se
 * autorresetea en vez de quedarse colgado sin que nadie se entere.
 *
 * LED de estado (pin 13, LED "L" de la placa): parpadea en un código de
 * N veces seguido de una pausa larga, N = código de estado actual (ver
 * STATUS_* en z21_protocol.h). Parpadeo de 1 = todo bien.
 *
 * VERSIONADO INTERNO: MEGA_FW_VERSION_MAJOR/MINOR en z21_protocol.h es
 * nuestra propia versión de firmware (no la que se declara a la app Z21),
 * para saber qué está cargado en el chip. Se manda en el heartbeat y el
 * ESP la muestra en la web.
 *
 * DEPURACIÓN POR USB: con el DIP switch en la fila "Special solution" de
 * la tabla del fabricante (1,2,3,4 ON, 5-7 OFF, más el selector físico en
 * RXD3/TXD3), el USB queda cableado directo a este Mega (Serial0) A LA VEZ
 * que el enlace Serial3 con el ESP sigue activo — así se puede ver este
 * log sin desconectar el enlace real. Ver AGENT.md, sección Diagnóstico.
 *
 * Enlace con el ESP8266: Serial3, framing [SYNC0][SYNC1][TYPE][LEN][payload][CHK]
 * (ver z21_protocol.h). Antes de que el ESP levante el servicio Z21 por
 * UDP, hay un pequeño handshake: el Mega manda HELLO hasta que el ESP
 * contesta con NET_INFO (modo STA/AP, IP, SSID — pensado para la futura
 * pantalla TFT), y el Mega confirma con SYNC_ACK. Mientras dura el
 * handshake el LED parpadea en código 5 (STATUS_SYNCING). Si el ESP no
 * contesta en unos segundos, seguimos igual en modo degradado — nunca
 * bloqueado esperando para siempre. Ver AGENT.md, "Sincronización inicial".
 *
 * PANTALLA (display_*.h/.cpp, en esta misma carpeta): a partir de v0.5
 * el Mega pinta en la pantalla 3.5" TFT una cabecera con el estado del
 * enlace ESP, los datos de red, los contadores de frames y el estado
 * global de salud, más un panel de log con las últimas líneas de
 * eventos (sync, track power, e-stop, datasets inválidos...). Sin
 * encoder todavía, así que es una única vista fija en apaisado — el
 * módulo está deliberadamente fragmentado en varios ficheros pequeños
 * (driver/layout/status/log/manager) en vez de un solo fichero, y
 * mega_z21.ino solo depende de la fachada display_manager.h. Ver cada
 * fichero display_*.h/.cpp para su propio historial de cambios.
 */

#include <avr/wdt.h>
#include <EEPROM.h>
#include "protocol/z21_protocol.h"
#include "traction/traction_config.h"  // TRACTION_BACKEND_SELECTED — cambiar de backend SOLO aquí
#include "traction/traction_types.h"   // LocoState/TrackState — formato compartido, ver AGENT.md
#include "traction/traction_backend.h" // ITractionBackend — el núcleo Z21 SOLO conoce esta interfaz
#include "traction/traction_backend_dummy.h"     // backend sin hardware, para validar el handshake Z21
#include "traction/traction_backend_xpressnet.h" // backend real, esclavo XpressNet de la MultiMaus
                                         // (contenido vacío si no está seleccionado, ver ese .h)
#include "display/display_manager.h" // pantalla 3.5" TFT: estado + log de comunicación
                             // (ver AGENT.md, "Pantalla 3.5 TFT", y
                             // docs/Z21_EMULATOR_SPEC.md sección 9)

// El núcleo Z21 de este sketch habla SOLO con `traction` (ITractionBackend,
// ver traction_backend.h) — de qué backend concreto se trate se decide en
// traction_config.h y es invisible desde aquí en adelante.
#if TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_XPRESSNET
XpressNetTractionBackend traction;
#else
DummyTractionBackend traction;
#endif

LocoState lastLocoState;
bool hasLastLocoState = false;

#define LINK_SERIAL Serial3
#define LINK_BAUD 115200
#define WATCHDOG_TIMEOUT WDTO_4S
#define LED_PIN 13
#define DEBUG_SERIAL Serial
#define DEBUG_BAUD 115200

// ---------------------------------------------------------------------
// Framing con el ESP8266: [TYPE(1)][LEN(1)][payload]
// ---------------------------------------------------------------------
#define LINK_BUF_SIZE 256
uint8_t linkBuf[LINK_BUF_SIZE];

// Throttle sencillo para no inundar el log circular de la pantalla si hay
// una ráfaga de errores (mismo patrón que evLogThrottle() en
// esp8266_wifi.ino, no tocar uno sin revisar el otro): un error real se
// sigue viendo igual, solo se limita la frecuencia de líneas repetidas.
bool displayLogThrottle(unsigned long &lastMs, unsigned long minIntervalMs) {
  unsigned long now = millis();
  if (lastMs != 0 && (now - lastMs) < minIntervalMs) return false;
  lastMs = now;
  return true;
}

// Contadores de diagnóstico DE LA VENTANA ACTUAL (desde el último
// heartbeat, ver sendHeartbeatIfDue): se resetean cada HEARTBEAT_INTERVAL_MS
// igual que las estadísticas de ciclo, en vez de acumular desde el
// arranque hasta saturar en 255 y quedarse ahí para siempre sin aportar
// más información. La detección de "esto ha pasado ALGUNA VEZ desde que
// arrancó" (para STATUS_NO_FRAMES_EVER) vive aparte, en
// everReceivedGoodZ21Frame (ver más abajo) — un latch que nunca se
// resetea, para no confundir "sin frames en este segundo concreto" con
// "nunca ha llegado ni un solo frame válido en toda la vida del firmware".
// Cada error individual, además, queda registrado en el log de pantalla
// en el momento en que ocurre (ver tryReadFrameFromESP y el dispatch en
// loop()) — así se puede reconstruir el historial real en vez de
// depender solo de un contador que se congela al saturar.
uint8_t framesRxOk = 0;
uint8_t framesRxBad = 0;
bool everReceivedGoodZ21Frame = false; // true en cuanto llega el primer Z21 válido; nunca se resetea
unsigned long totalRawBytesFromESP = 0; // TODO byte recibido, haya frame o no

void sendFrameToESP(uint8_t type, const uint8_t *payload, uint8_t len) {
  uint8_t chk = type ^ len;
  for (uint8_t i = 0; i < len; i++) chk ^= payload[i];
  LINK_SERIAL.write(LINK_SYNC_BYTE_0);
  LINK_SERIAL.write(LINK_SYNC_BYTE_1);
  LINK_SERIAL.write(type);
  LINK_SERIAL.write(len);
  LINK_SERIAL.write(payload, len);
  LINK_SERIAL.write(chk);
}

// Máquina de estados no bloqueante con sync bytes + checksum de framing.
// NO consume ningún byte "a ciegas" hasta que el frame completo esté
// disponible (mismo principio que la versión anterior, ver AGENT.md), pero
// además: si el checksum no cuadra, el frame se descarta y se vuelve a
// LINK_WAIT_SYNC0 en vez de asumir que el siguiente byte es un TYPE válido.
// Así, un byte corrupto/perdido en el enlace físico ya NO desincroniza la
// máquina de estados para siempre (antes, un solo byte malo hacía que todo
// lo que llegase después se interpretase con el offset equivocado — eso
// era la causa real de los frames "type=0x01 len=0x00" repetidos).
enum LinkState { LINK_WAIT_SYNC0, LINK_WAIT_SYNC1, LINK_WAIT_TYPE, LINK_WAIT_LEN, LINK_WAIT_PAYLOAD, LINK_WAIT_CHK };
LinkState linkState = LINK_WAIT_SYNC0;
uint8_t linkPendingType = 0;
uint8_t linkPendingLen = 0;
uint8_t linkPendingCount = 0;
uint8_t linkPendingChk = 0;
uint8_t linkPendingBuf[LINK_BUF_SIZE];
uint8_t framesRxChkFail = 0; // frames descartados por checksum de framing (distinto de framesRxBad)

// IMPORTANTE: maxLen es uint16_t, NO uint8_t. Se llama con LINK_BUF_SIZE=256,
// y 256 no cabe en un uint8_t (se trunca a 256 % 256 = 0) — este era el bug
// real detrás del "100% de frames Z21 corruptos" que investigamos al
// principio: maxLen valía 0 en TODAS las llamadas, así que outLen salía
// siempre 0 sin importar el frame real recibido, y por eso todo se contaba
// como framesRxBad (len<4). No era un problema eléctrico del enlace.
//
// NOTA DE FUSIÓN (unificación de las dos ramas, ver historial): esta rama
// había perdido el fix (volvía a uint8_t) al añadir el módulo de pantalla;
// se restaura aquí. Mantener SIEMPRE este comentario junto a la firma para
// que no se vuelva a revertir por error en un futuro refactor.
bool tryReadFrameFromESP(uint8_t &outType, uint8_t *buf, uint16_t maxLen, uint8_t &outLen) {
  while (LINK_SERIAL.available() > 0) {
    uint8_t b = LINK_SERIAL.read();
    totalRawBytesFromESP++;

    switch (linkState) {
      case LINK_WAIT_SYNC0:
        if (b == LINK_SYNC_BYTE_0) linkState = LINK_WAIT_SYNC1;
        // si no es SYNC0, se descarta y seguimos buscando en el flujo
        break;

      case LINK_WAIT_SYNC1:
        if (b == LINK_SYNC_BYTE_1) {
          linkState = LINK_WAIT_TYPE;
        } else if (b != LINK_SYNC_BYTE_0) {
          // no era el segundo byte de sync ni tampoco un posible SYNC0
          // nuevo: volvemos a buscar desde cero
          linkState = LINK_WAIT_SYNC0;
        }
        // si b == SYNC0 nos quedamos en LINK_WAIT_SYNC1 (podría ser el
        // verdadero SYNC0 de un frame que empieza justo aquí)
        break;

      case LINK_WAIT_TYPE:
        linkPendingType = b;
        linkPendingChk = b;
        linkState = LINK_WAIT_LEN;
        break;

      case LINK_WAIT_LEN:
        linkPendingLen = b;
        linkPendingChk ^= b;
        linkPendingCount = 0;
        linkState = (linkPendingLen == 0) ? LINK_WAIT_CHK : LINK_WAIT_PAYLOAD;
        break;

      case LINK_WAIT_PAYLOAD:
        if (linkPendingCount < LINK_BUF_SIZE) {
          linkPendingBuf[linkPendingCount] = b;
        }
        linkPendingChk ^= b;
        linkPendingCount++;
        if (linkPendingCount >= linkPendingLen) {
          linkState = LINK_WAIT_CHK;
        }
        break;

      case LINK_WAIT_CHK:
        linkState = LINK_WAIT_SYNC0; // pase lo que pase, volvemos a buscar sync
        if (b != linkPendingChk) {
          framesRxChkFail = (framesRxChkFail < 255) ? framesRxChkFail + 1 : 255;
          static unsigned long lastChkFailLogMs = 0;
          if (displayLogThrottle(lastChkFailLogMs, 500)) {
            displayLogf("Fallo checksum framing (%u en esta ventana)", (unsigned)framesRxChkFail);
          }
          break; // checksum no cuadra: frame descartado, no devolvemos nada
        }
        outType = linkPendingType;
        outLen = (linkPendingLen < maxLen) ? linkPendingLen : maxLen;
        for (uint8_t i = 0; i < outLen; i++) buf[i] = linkPendingBuf[i];
        return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------
// Construcción de datasets Z21 (DataLen 2B LE + Header 2B LE + Data)
// ---------------------------------------------------------------------
uint8_t respBuf[LINK_BUF_SIZE];

void sendDataset(uint16_t header, const uint8_t *data, uint8_t dataLen) {
  uint16_t total = 4 + dataLen; // DataLen incluye los 4 bytes de cabecera
  respBuf[0] = total & 0xFF;
  respBuf[1] = (total >> 8) & 0xFF;
  respBuf[2] = header & 0xFF;
  respBuf[3] = (header >> 8) & 0xFF;
  for (uint8_t i = 0; i < dataLen; i++) respBuf[4 + i] = data[i];
  sendFrameToESP(FRAME_TYPE_Z21, respBuf, (uint8_t)total);
}

// Un dataset LAN_X va dentro de un dataset con Header=0x40; xData ya debe
// incluir el checksum final (XOR de XHeader..DBn).
void sendXDataset(const uint8_t *xData, uint8_t xLen) {
  sendDataset(0x40, xData, xLen);
}

uint8_t xorChecksum(const uint8_t *data, uint8_t len) {
  uint8_t cs = 0;
  for (uint8_t i = 0; i < len; i++) cs ^= data[i];
  return cs;
}

// ---------------------------------------------------------------------
// Sincronización inicial con el ESP (antes de que el ESP levante el UDP
// Z21, ver z21_protocol.h y AGENT.md). El Mega manda HELLO hasta que el
// ESP contesta con NET_INFO (modo STA/AP, IP, SSID); el Mega guarda esa
// info (para la futura pantalla TFT) y confirma con SYNC_ACK. Si no se
// completa en SYNC_TIMEOUT_MS, seguimos igualmente en modo degradado —
// nunca nos quedamos bloqueados para siempre esperando al ESP.
// ---------------------------------------------------------------------
bool synced = false;
bool syncDegraded = false; // true si se pasó al timeout sin completar el handshake
unsigned long syncStartMs = 0;
unsigned long lastHelloSentMs = 0;

uint8_t netInfoMode = 0xFF; // 0xFF = todavía no se conoce
uint32_t netInfoIp = 0;
uint32_t netInfoGateway = 0;
uint8_t netInfoMac[NET_INFO_MAC_LEN] = {0, 0, 0, 0, 0, 0};
char netInfoSsid[NET_INFO_SSID_MAXLEN + 1] = "";

void sendHello() {
  sendFrameToESP(FRAME_TYPE_HELLO, nullptr, 0);
}

void sendSyncAck() {
  sendFrameToESP(FRAME_TYPE_SYNC_ACK, nullptr, 0);
}

void handleNetInfo(const uint8_t *payload, uint8_t len) {
  netInfoMode = (len > 0) ? payload[0] : NET_INFO_MODE_AP;
  netInfoIp = 0;
  netInfoGateway = 0;
  memset(netInfoMac, 0, sizeof(netInfoMac));
  netInfoSsid[0] = '\0';

  if (len >= 5) {
    netInfoIp = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8) |
                ((uint32_t)payload[3] << 16) | ((uint32_t)payload[4] << 24);
  }

  if (len >= 9) {
    netInfoGateway = (uint32_t)payload[5] | ((uint32_t)payload[6] << 8) |
                      ((uint32_t)payload[7] << 16) | ((uint32_t)payload[8] << 24);
  }

  // Offset fijo tras modo(1)+ip(4)+gateway(4) = 9; la MAC ocupa
  // NET_INFO_MAC_LEN bytes justo despues.
  const uint8_t macOffset = 9;
  const uint8_t ssidLenOffset = macOffset + NET_INFO_MAC_LEN; // 15
  if (len >= ssidLenOffset) {
    memcpy(netInfoMac, &payload[macOffset], NET_INFO_MAC_LEN);
  }

  if (len >= (uint8_t)(ssidLenOffset + 1)) {
    uint8_t ssidLen = payload[ssidLenOffset];
    if (ssidLen > NET_INFO_SSID_MAXLEN) ssidLen = NET_INFO_SSID_MAXLEN;
    uint8_t ssidOffset = ssidLenOffset + 1;
    if ((uint16_t)(ssidOffset + ssidLen) > len) ssidLen = (len > ssidOffset) ? (len - ssidOffset) : 0;
    for (uint8_t i = 0; i < ssidLen; i++) netInfoSsid[i] = (char)payload[ssidOffset + i];
    netInfoSsid[ssidLen] = '\0';
  }

  synced = true;
  sendSyncAck();

  DEBUG_SERIAL.print(F("[SYNC] NET_INFO recibido: modo="));
  DEBUG_SERIAL.print(netInfoMode == NET_INFO_MODE_AP ? F("AP") : F("STA"));
  DEBUG_SERIAL.print(F(" ip="));
  DEBUG_SERIAL.print(netInfoIp & 0xFF); DEBUG_SERIAL.print('.');
  DEBUG_SERIAL.print((netInfoIp >> 8) & 0xFF); DEBUG_SERIAL.print('.');
  DEBUG_SERIAL.print((netInfoIp >> 16) & 0xFF); DEBUG_SERIAL.print('.');
  DEBUG_SERIAL.print((netInfoIp >> 24) & 0xFF);
  DEBUG_SERIAL.print(F(" gw="));
  DEBUG_SERIAL.print(netInfoGateway & 0xFF); DEBUG_SERIAL.print('.');
  DEBUG_SERIAL.print((netInfoGateway >> 8) & 0xFF); DEBUG_SERIAL.print('.');
  DEBUG_SERIAL.print((netInfoGateway >> 16) & 0xFF); DEBUG_SERIAL.print('.');
  DEBUG_SERIAL.print((netInfoGateway >> 24) & 0xFF);
  DEBUG_SERIAL.print(F(" mac="));
  for (uint8_t i = 0; i < NET_INFO_MAC_LEN; i++) {
    if (i > 0) DEBUG_SERIAL.print(':');
    if (netInfoMac[i] < 16) DEBUG_SERIAL.print('0');
    DEBUG_SERIAL.print(netInfoMac[i], HEX);
  }
  DEBUG_SERIAL.print(F(" ssid="));
  DEBUG_SERIAL.println(netInfoSsid);

  // El SSID, el gateway y la MAC no se muestran en la cabecera de estado
  // (no caben sin competir con el resto de campos, ver
  // display_status_panel.cpp), asi que quedan registrados aqui, en el log
  // de comunicacion visible en la pantalla y en /log del ESP.
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           netInfoMac[0], netInfoMac[1], netInfoMac[2], netInfoMac[3], netInfoMac[4], netInfoMac[5]);
  displayLogf("NET_INFO: %s ssid=%s gw=%lu.%lu.%lu.%lu mac=%s",
              netInfoMode == NET_INFO_MODE_AP ? "AP" : "STA", netInfoSsid,
              netInfoGateway & 0xFF, (netInfoGateway >> 8) & 0xFF,
              (netInfoGateway >> 16) & 0xFF, (netInfoGateway >> 24) & 0xFF, macStr);
}

// Se llama en cada vuelta de loop() mientras !synced. No bloquea nunca:
// si el ESP tarda o no contesta, a los SYNC_TIMEOUT_MS seguimos igual.
void runSyncStep() {
  unsigned long now = millis();
  if (syncStartMs == 0) syncStartMs = now;

  if (lastHelloSentMs == 0 || (now - lastHelloSentMs) >= SYNC_HELLO_INTERVAL_MS) {
    sendHello();
    lastHelloSentMs = now;
  }

  if (!synced && (now - syncStartMs) > SYNC_TIMEOUT_MS) {
    synced = true;
    syncDegraded = true; // sin info de red, pero no nos bloqueamos más
    DEBUG_SERIAL.println(F("[SYNC] Timeout esperando NET_INFO del ESP — sigo en modo degradado"));
    displayLogF(F("Timeout sync ESP: modo degradado"));
  }
}

// ---------------------------------------------------------------------
// DUMMY: handshake de reconocimiento (prioridad nº1, ver spec sección 5)
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Número de serie: PERSISTENTE en EEPROM, no un valor mágico hardcodeado.
// Investigado contra la librería z21 de referencia de Philipp Gahtow
// (https://github.com/Digital-MoBa/Z21, base de facto de casi todos los
// clones DIY reales que sí son reconocidos por la app oficial): esa
// librería tampoco usa un número "especial" ni de un rango reservado —
// simplemente lee/escribe 2 bytes desde almacenamiento persistente
// (EEPROM en AVR, NVS en ESP32) y deja los 2 bytes altos siempre a 0x00.
// Esto descarta razonablemente la teoría de "necesita un rango de serie
// válido de fábrica": si así fuera, ese proyecto de referencia —
// ampliamente usado y confirmado funcionando con la app— no funcionaría
// con un número arbitrario tampoco.
// Primer arranque: la EEPROM de un ATmega virgen viene a 0xFF en todos
// sus bytes, así que 0xFFFF (o 0x0000, por si alguien la borró a ceros)
// se interpreta como "sin inicializar" y se genera un valor por defecto
// una única vez.
#define EEPROM_ADDR_SERIAL_LSB 0
#define EEPROM_ADDR_SERIAL_MSB 1
#define DEFAULT_SERIAL_NUMBER 5421 // arbitrario, ver comentario arriba

uint16_t loadOrInitSerialNumber() {
  uint8_t lsb = EEPROM.read(EEPROM_ADDR_SERIAL_LSB);
  uint8_t msb = EEPROM.read(EEPROM_ADDR_SERIAL_MSB);
  uint16_t serial = (uint16_t)lsb | ((uint16_t)msb << 8);
  if (serial == 0xFFFF || serial == 0x0000) {
    serial = DEFAULT_SERIAL_NUMBER;
    EEPROM.write(EEPROM_ADDR_SERIAL_LSB, serial & 0xFF);
    EEPROM.write(EEPROM_ADDR_SERIAL_MSB, (serial >> 8) & 0xFF);
  }
  return serial;
}

uint16_t megaSerialNumber = 0; // se rellena en setup() con loadOrInitSerialNumber()

void handleGetSerialNumber() {
  uint8_t data[4] = {
    (uint8_t)(megaSerialNumber & 0xFF),
    (uint8_t)((megaSerialNumber >> 8) & 0xFF),
    0x00, // los 2 bytes altos van siempre a 0, igual que en la librería de referencia
    0x00
  };
  sendDataset(LAN_GET_SERIAL_NUMBER, data, 4);
}

void handleGetHwInfo() {
  uint32_t hwType = D_HWT_Z21_NEW; // 0x00000201, ver z21_protocol.h
  // Reportar una versión de firmware moderna y coherente con la app.
  // El formato sigue siendo little-endian como en el PDF oficial.
  uint32_t fwVersion = 0x00000142UL; // V1.42
  uint8_t data[8] = {
    (uint8_t)(hwType & 0xFF), (uint8_t)((hwType >> 8) & 0xFF),
    (uint8_t)((hwType >> 16) & 0xFF), (uint8_t)((hwType >> 24) & 0xFF),
    (uint8_t)(fwVersion & 0xFF), (uint8_t)((fwVersion >> 8) & 0xFF),
    (uint8_t)((fwVersion >> 16) & 0xFF), (uint8_t)((fwVersion >> 24) & 0xFF)
  };
  sendDataset(LAN_GET_HWINFO, data, 8);
}

void handleGetCode() {
  // 0x00 = Z21_NO_LOCK (sin restricciones)
  // 0x01 = Indica a la app que las características extendidas/red están activas
  uint8_t data[1] = { Z21_NO_LOCK};
  sendDataset(LAN_GET_CODE, data, 2);
}

// LAN_GET_BROADCASTFLAGS / LAN_SET_BROADCASTFLAGS (PDF oficial, secciones
// 2.16-2.17): forman parte de la secuencia estándar de conexión de la
// app. TODO: esta versión dummy no lleva un almacén real de flags por
// cliente (IP+puerto) — se acepta cualquier SET sin más y el GET siempre
// devuelve 0 (ningún broadcast activo). Cuando haya backend de tracción
// real habrá que trackear esto por cliente para poder mandar de verdad
// LAN_X_BC_* / LAN_SYSTEMSTATE_DATACHANGED como broadcast asíncrono, no
// solo como respuesta a un GET explícito.
// TODO real: por cliente (IP+puerto), como hace la librería de
// referencia (Digital-MoBa/Z21, addIPToSlot/getLocalBcFlag) — de momento
// un único valor global, suficiente mientras solo haya un cliente Z21
// típico (la app) hablando con el ESP. Ya no está hardcodeado a 0: se
// guarda de verdad lo último que la app mandó.
uint32_t dummyBroadcastFlags = 0;

void handleSetBroadcastFlags(const uint8_t *data, uint8_t dataLen) {
  if (dataLen < 4) return;
  dummyBroadcastFlags = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

void handleGetBroadcastFlags() {
  uint8_t data[4] = {
    (uint8_t)(dummyBroadcastFlags & 0xFF), (uint8_t)((dummyBroadcastFlags >> 8) & 0xFF),
    (uint8_t)((dummyBroadcastFlags >> 16) & 0xFF), (uint8_t)((dummyBroadcastFlags >> 24) & 0xFF)
  };
  sendDataset(LAN_GET_BROADCASTFLAGS, data, 4);
}

// ---------------------------------------------------------------------
// Estado de vía: SIEMPRE viene de traction.getTrackState() (backend
// activo, ver selector arriba), nunca de una variable propia del núcleo
// Z21 — así LAN_X_GET_STATUS y LAN_SYSTEMSTATE_GETDATA responden de
// forma coherente entre sí y con el backend real, sea cual sea.
// ---------------------------------------------------------------------
uint8_t buildCentralStateByte() {
  TrackState ts = traction.getTrackState();
  uint8_t s = 0;
  if (!ts.powerOn) s |= CS_TRACK_VOLTAGE_OFF;
  if (ts.emergencyStop) s |= CS_EMERGENCY_STOP;
  if (ts.shortCircuit) s |= CS_SHORT_CIRCUIT;
  if (ts.serviceModeActive) s |= CS_PROGRAMMING_MODE_ACTIVE;
  return s;
}

// LAN_SYSTEMSTATE_GETDATA / LAN_SYSTEMSTATE_DATACHANGED (PDF oficial,
// secciones 2.18-2.19): el propio PDF indica que el login de la app se
// hace de forma implícita con el PRIMER comando que mande, dando como
// ejemplo justo este — es decir, la app puede apoyarse en esta respuesta
// (además de LAN_GET_SERIAL_NUMBER/HWINFO) para decidir si la central es
// válida. SystemState son 16 bytes fijos (ver z21_protocol.h, bitmask
// CS_*); usa el mismo buildCentralStateByte() (estado real del backend
// de tracción activo) que declara handleXGetStatus() (X-Header 0x62/
// 0x22), para que ambas respuestas sean coherentes entre sí.
void handleSystemStateGetData() {
  uint8_t data[16];

  uint16_t jitterSlow = (uint16_t)((millis() / 3000) % 5); // 0..4, cambia cada 3s
  int16_t temperature = 24 + (int16_t)jitterSlow;          // 24..28 °C
  uint16_t supplyMv = 18000 + (jitterSlow * 20);          // 18000..18080 mV, para que la app vea vía activa

  uint16_t mainCurrentMa = 0;
  TrackState ts = traction.getTrackState();
  if (ts.powerOn && !ts.emergencyStop) {
    uint16_t jitterFast = (uint16_t)((millis() / 500) % 30);
    mainCurrentMa = 300 + jitterFast;
  }

  data[0] = mainCurrentMa & 0xFF; data[1] = (mainCurrentMa >> 8) & 0xFF;
  data[2] = 0x00; data[3] = 0x00;
  data[4] = mainCurrentMa & 0xFF; data[5] = (mainCurrentMa >> 8) & 0xFF;
  data[6] = temperature & 0xFF; data[7] = (temperature >> 8) & 0xFF;
  data[8] = supplyMv & 0xFF; data[9] = (supplyMv >> 8) & 0xFF;
  data[10] = supplyMv & 0xFF; data[11] = (supplyMv >> 8) & 0xFF;
  data[12] = buildCentralStateByte();
  data[13] = 0x00;
  data[14] = 0x00;
  data[15] = CAP_DCC | CAP_LOCO_CMDS;
  sendDataset(LAN_SYSTEMSTATE_DATACHANGED, data, 16);
}

void handleXGetVersion() {
  // Verificado contra el PDF oficial (sección 2.3): X-Header 0x63, DB0 0x21,
  // DB1=XBUS_VER (0x30 = V3.0), DB2=CMDST_ID (0x12 = familia de dispositivo Z21)
  uint8_t x[5];
  x[0] = 0x63; // XHeader respuesta
  x[1] = 0x21; // DB0
  x[2] = 0x36; // DB1: XBUS_VER 3.6 (CRÍTICO: en lugar de 0x30)
  x[3] = 0x12; // DB2: CMDST_ID (Familia Z21)
  x[4] = xorChecksum(x, 4);
  sendXDataset(x, 5);
}

void handleXGetStatus() {
  // X-Header 0x62, DB0 0x22, Status (real: refleja el TrackState del
  // backend de tracción activo, ver buildCentralStateByte), checksum
  uint8_t x[4];
  x[0] = 0x62;
  x[1] = 0x22;
  x[2] = buildCentralStateByte();
  x[3] = xorChecksum(x, 3);
  sendXDataset(x, 4);
}

// LAN_X_GET_FIRMWARE_VERSION (PDF oficial, sección 2.15): X-Header 0xF1,
// DB0 0x0A en la petición; X-Header 0xF3, DB0 0x0A, DB1=V_MSB, DB2=V_LSB
// (BCD) en la respuesta. Comando ESTÁNDAR documentado, forma parte de la
// secuencia de identificación inicial de la app junto con LAN_GET_HWINFO
// y LAN_X_GET_VERSION — hasta ahora no existía ningún 'case' para XHeader
// 0xF1 en handleDataset(), así que caía en el 'default' y NO se contestaba
// nada. Es el candidato más probable para el aviso "central extranjera
// detectada": una app que hace esta petición y nunca recibe respuesta
// tiene un motivo objetivo para desconfiar de la central. Se usa la misma
// versión dummy "V1.40" ya declarada en handleGetHwInfo() para ser
// consistentes entre los dos comandos de versión.
void handleXGetFirmwareVersion() {
  uint8_t x[5];
  x[0] = 0xF3;
  x[1] = 0x0A;
  x[2] = 0x01; // V_MSB (BCD): "1"
  x[3] = 0x40; // V_LSB (BCD): "40" -> V1.40
  x[4] = xorChecksum(x, 4);
  sendXDataset(x, 5);
}

// ---------------------------------------------------------------------
// Comandos v1 de tracción y locomotoras: el núcleo Z21 solo traduce
// entre el formato de datagrama LAN_X y llamadas a `traction`
// (ITractionBackend) — qué backend sea de verdad (dummy o XpressNet) es
// invisible desde aquí, ver el selector al principio del sketch.
// ---------------------------------------------------------------------
void handleXSetTrackPowerOff() {
  traction.setTrackPower(false);
  uint8_t x[3] = { 0x61, 0x00, 0x00 };
  x[2] = xorChecksum(x, 2);
  sendXDataset(x, 3); // LAN_X_BC_TRACK_POWER_OFF
  displayLogF(F("Track power OFF"));
}

void handleXSetTrackPowerOn() {
  // Verificado contra el PDF (sección 2.6): este comando también termina
  // la parada de emergencia y el modo de programación si estuvieran
  // activos — cada backend concreto de ITractionBackend::setTrackPower
  // replica esa misma regla (ver traction_backend_dummy.h /
  // traction_backend_xpressnet.cpp).
  traction.setTrackPower(true);
  uint8_t x[3] = { 0x61, 0x01, 0x00 };
  x[2] = xorChecksum(x, 2);
  sendXDataset(x, 3); // LAN_X_BC_TRACK_POWER_ON
  displayLogF(F("Track power ON"));
}

void sendLocoInfoResponse(uint8_t adrMsbRaw, uint8_t adrLsb, const LocoState *loco) {
  uint8_t x[10];
  x[0] = 0xEF; // X-Header LAN_X_LOCO_INFO
  x[1] = adrMsbRaw; // la app ignora los 2 bits altos, se devuelve tal cual se pidió
  x[2] = adrLsb;
  x[3] = loco->stepsCode;
  x[4] = loco->speedByte;
  x[5] = loco->f0to4;
  x[6] = loco->f5to12;
  x[7] = loco->f13to20;
  x[8] = loco->f21to28;
  x[9] = xorChecksum(x, 9);
  sendXDataset(x, 10);
}

void handleXGetLocoInfo(const uint8_t *reqData, uint8_t reqLen) {
  // Verificado contra el PDF (sección 4.4): la respuesta mínima necesita
  // DB0-DB7 (8 bytes) + XHeader + checksum = 10 bytes en total.
  if (reqLen < 4) return; // XHeader 0xE3 + DB0(0xF0) + AddrH + AddrL esperado
  uint16_t addr = ((uint16_t)(reqData[2] & 0x3F) << 8) | reqData[3];
  // Dispara un refresco desde el bus real si el backend es asíncrono (en
  // el dummy no hace nada, ver ITractionBackend::requestLocoRefresh). La
  // respuesta a la app se contesta YA con el último dato conocido — no
  // se puede hacer esperar a la app al tiempo de vuelta del bus físico,
  // ver traction_backend.h.
  traction.requestLocoRefresh(addr);
  const LocoState *loco = traction.getLocoState(addr);
  if (loco != nullptr) {
    lastLocoState = *loco;
    hasLastLocoState = true;
  }
  sendLocoInfoResponse(reqData[2], reqData[3], loco);
}

void handleXSetLocoDriveOrFunction(const uint8_t *data, uint8_t dataLen) {
  if (dataLen < 4) return; // XHeader + DB0 + Adr_MSB + Adr_LSB como mínimo
  uint8_t db0 = data[1];
  uint16_t addr = ((uint16_t)(data[2] & 0x3F) << 8) | data[3];

  if ((db0 & 0xF0) == 0x10 && dataLen >= 5) {
    // LAN_X_SET_LOCO_DRIVE (PDF sección 4.2)
    uint8_t stepsCode;
    if (db0 == 0x10) stepsCode = 0;       // 14 pasos
    else if (db0 == 0x12) stepsCode = 2;  // 28 pasos
    else stepsCode = 4;                   // 128 pasos (0x13 y variantes)
    traction.setLocoDrive(addr, stepsCode, data[4]);
    // Confirmación visible en el log de pantalla — sin esto, un cambio de
    // velocidad/sentido solo se reflejaba en la fila "Loco#..." de la
    // cabecera (fácil de no notar), a diferencia de track power ON/OFF que
    // sí logueaba explícitamente (ver handleXSetTrackPowerOff/On).
    displayLogf("Loco %u: V=%u %s", (unsigned)addr,
                (unsigned)(data[4] & 0x7F),
                (data[4] & 0x80) ? "FWD" : "REV");
  } else if (db0 == 0xF8 && dataLen >= 5) {
    // LAN_X_SET_LOCO_FUNCTION (PDF sección 4.3.1): DB3 = TTNNNNNN
    uint8_t index = data[4] & 0x3F;
    uint8_t type = (data[4] >> 6) & 0x03;
    if (type != 0b11) { // 0b11 = "no permitido" según el PDF
      traction.setLocoFunction(addr, index, static_cast<FunctionOp>(type));
      static const char *kFuncOpLabel[3] = { "OFF", "ON", "TOGGLE" };
      displayLogf("Loco %u: F%u %s", (unsigned)addr, (unsigned)index,
                  kFuncOpLabel[type]);
    }
  } else if (db0 == 0x20 && dataLen >= 5) {
    // LAN_X_SET_LOCO_FUNCTION_GROUP grupo 1, F0-F4 (PDF 4.3.2)
    traction.setLocoFunctionGroup(addr, FunctionGroup::F0toF4, data[4]);
    displayLogf("Loco %u: F0-F4 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  } else if (db0 == 0x21 && dataLen >= 5) {
    traction.setLocoFunctionGroup(addr, FunctionGroup::F5toF8, data[4]); // grupo 2
    displayLogf("Loco %u: F5-F8 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  } else if (db0 == 0x22 && dataLen >= 5) {
    traction.setLocoFunctionGroup(addr, FunctionGroup::F9toF12, data[4]); // grupo 3
    displayLogf("Loco %u: F9-F12 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  } else if (db0 == 0x23 && dataLen >= 5) {
    traction.setLocoFunctionGroup(addr, FunctionGroup::F13toF20, data[4]); // grupo 4
    displayLogf("Loco %u: F13-F20 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  } else if (db0 == 0x28 && dataLen >= 5) {
    traction.setLocoFunctionGroup(addr, FunctionGroup::F21toF28, data[4]); // grupo 5
    displayLogf("Loco %u: F21-F28 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  }
  // TODO: grupos 6-10 (F29-F68, PDF 4.3.2) y LAN_X_SET_LOCO_BINARY_STATE
  // (4.3.3) quedan fuera todavía — LocoState no tiene campo reservado
  // para funciones >F28 (ver traction_types.h).

  const LocoState *updatedLoco = traction.getLocoState(addr);
  if (updatedLoco != nullptr) {
    lastLocoState = *updatedLoco;
    hasLastLocoState = true;
  }
  sendLocoInfoResponse(data[2], data[3], updatedLoco);
}

// ---------------------------------------------------------------------
// Accesorios (agujas/señales/desacopladores/descarriladores biestables,
// PDF sección 5 "Switching") — ver AccessoryState en traction_types.h y
// las asunciones de conversión documentadas en traction_backend_
// xpressnet.h/.cpp.
// ---------------------------------------------------------------------
void sendTurnoutInfoResponse(uint8_t adrMsb, uint8_t adrLsb, const AccessoryState *acc) {
  // Verificado contra el PDF (sección 5.3): X-Header 0x43, DB0=AdrMSB,
  // DB1=AdrLSB, DB2=000000ZZ, XOR — 5 bytes en total.
  uint8_t x[5];
  x[0] = 0x43; // X-Header LAN_X_TURNOUT_INFO
  x[1] = adrMsb;
  x[2] = adrLsb;
  x[3] = (uint8_t)acc->position & 0x03;
  x[4] = xorChecksum(x, 4);
  sendXDataset(x, 5);
}

void handleXGetTurnoutInfo(const uint8_t *data, uint8_t dataLen) {
  // Verificado contra el PDF (sección 5.1): XHeader(1)+DB0=AdrMSB+
  // DB1=AdrLSB, sin más datos.
  if (dataLen < 3) return;
  // Dirección de accesorio: 16 bits COMPLETOS, sin el enmascarado "& 0x3F"
  // que sí llevan las direcciones de loco (PDF 5.1: "Function address =
  // (FAdr_MSB << 8) + FAdr_LSB" — comparar con 4.4 para la diferencia).
  uint16_t addr = ((uint16_t)data[1] << 8) | data[2];
  traction.requestTurnoutRefresh(addr);
  const AccessoryState *acc = traction.getTurnoutState(addr);
  sendTurnoutInfoResponse(data[1], data[2], acc);
}

void handleXSetTurnout(const uint8_t *data, uint8_t dataLen) {
  // Verificado contra el PDF (sección 5.2): XHeader(1)+DB0=AdrMSB+
  // DB1=AdrLSB+DB2=10Q0A00P, sin más datos.
  if (dataLen < 4) return;
  uint16_t addr = ((uint16_t)data[1] << 8) | data[2];
  uint8_t db2 = data[3];
  bool activate = (db2 & 0x08) != 0; // A: 0=desactivar salida, 1=activarla
  bool output = (db2 & 0x01) != 0;   // P: 0=salida 1, 1=salida 2
  // Q (bit5, "queue de comandos", desde Z21 FW V1.24) se ignora a
  // propósito: esta versión no implementa cola de accesorios, todo
  // comando se ejecuta de inmediato (comportamiento Q=0, compatible con
  // versiones anteriores del PDF).
  traction.setTurnout(addr, output, activate);
  displayLogf("Accesorio %u: salida %u %s", (unsigned)addr,
              output ? 2 : 1, activate ? "ON" : "OFF");
  // PDF sección 5.2: "Reply from Z21: No standard answer, 5.3
  // LAN_X_TURNOUT_INFO to subscribed clients" — esta versión, igual que
  // ya hace sendLocoInfoResponse tras LAN_X_SET_LOCO_DRIVE/FUNCTION, no
  // distingue clientes suscritos vía broadcast flags (ver
  // handleSetBroadcastFlags) y manda siempre la info actualizada.
  const AccessoryState *acc = traction.getTurnoutState(addr);
  sendTurnoutInfoResponse(data[1], data[2], acc);
}

void handleXSetStop() {
  traction.emergencyStopAll();
  uint8_t x[3] = { 0x81, 0x00, 0x00 };
  x[2] = xorChecksum(x, 2);
  sendXDataset(x, 3); // LAN_X_BC_STOPPED
  displayLogF(F("*** PARADA DE EMERGENCIA ***"));
}

void handleCanDetector(const uint8_t *data, uint8_t dataLen) {
  if (dataLen < 2) return;
  
  // Si la app pregunta por el estado del detector CAN (0x11):
  if (data[0] == 0x11) {
    uint8_t resp[10] = { 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    sendDataset(LAN_CAN_DETECTOR, resp, 10);
  }
}

void handleGetCommunicationInfo() {
  // Devuelve BroadcastFlags (4 bytes) + Puerto (2 bytes)
  uint8_t data[8] = {
    (uint8_t)(dummyBroadcastFlags & 0xFF), 
    (uint8_t)((dummyBroadcastFlags >> 8) & 0xFF),
    (uint8_t)((dummyBroadcastFlags >> 16) & 0xFF), 
    (uint8_t)((dummyBroadcastFlags >> 24) & 0xFF),
    0x69, 0x52, // Puerto 21105 en Little Endian (0x5269)
    0x00, 0x00
  };
  sendDataset(0x12, data, 8);
}

// ---------------------------------------------------------------------
// Diagnóstico: RAM libre, tiempos de ciclo, heartbeat periódico al ESP
// ---------------------------------------------------------------------
// Free RAM en AVR: truco estándar con el puntero del heap vs el stack
int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

unsigned long lastLoopMicros = 0;
unsigned long cycleSumUs = 0;   // para media móvil simple
uint16_t cycleSampleCount = 0;
uint16_t cycleMaxUs = 0;
unsigned long lastHeartbeatMs = 0;

void trackCycleTime() {
  unsigned long now = micros();
  if (lastLoopMicros != 0) {
    unsigned long delta = now - lastLoopMicros; // ok incluso si desborda micros()
    cycleSumUs += delta;
    cycleSampleCount++;
    if (delta > cycleMaxUs) cycleMaxUs = (delta > 0xFFFF) ? 0xFFFF : (uint16_t)delta;
  }
  lastLoopMicros = now;
}

void sendHeartbeatIfDue() {
  unsigned long nowMs = millis();
  if (nowMs - lastHeartbeatMs < HEARTBEAT_INTERVAL_MS) return;
  lastHeartbeatMs = nowMs;

  uint32_t uptimeMs = nowMs;
  uint32_t cycleAvgUs = (cycleSampleCount > 0) ? (cycleSumUs / cycleSampleCount) : 0;
  uint16_t freeRamBytes = (uint16_t)freeRam();

  uint8_t hb[HEARTBEAT_PAYLOAD_LEN];
  hb[0] = uptimeMs & 0xFF; hb[1] = (uptimeMs >> 8) & 0xFF;
  hb[2] = (uptimeMs >> 16) & 0xFF; hb[3] = (uptimeMs >> 24) & 0xFF;
  hb[4] = cycleAvgUs & 0xFF; hb[5] = (cycleAvgUs >> 8) & 0xFF;
  hb[6] = (cycleAvgUs >> 16) & 0xFF; hb[7] = (cycleAvgUs >> 24) & 0xFF;
  hb[8] = cycleMaxUs & 0xFF; hb[9] = (cycleMaxUs >> 8) & 0xFF;
  hb[10] = freeRamBytes & 0xFF; hb[11] = (freeRamBytes >> 8) & 0xFF;
  hb[12] = framesRxOk;
  hb[13] = framesRxBad;
  hb[14] = MEGA_FW_VERSION_MAJOR;
  hb[15] = MEGA_FW_VERSION_MINOR;
  hb[16] = currentStatusCode();
  sendFrameToESP(FRAME_TYPE_HEARTBEAT, hb, HEARTBEAT_PAYLOAD_LEN);

  DEBUG_SERIAL.print(F("[HB] uptime="));
  DEBUG_SERIAL.print(uptimeMs);
  DEBUG_SERIAL.print(F("ms cicloAvg="));
  DEBUG_SERIAL.print(cycleAvgUs);
  DEBUG_SERIAL.print(F("us cicloMax="));
  DEBUG_SERIAL.print(cycleMaxUs);
  DEBUG_SERIAL.print(F("us ram="));
  DEBUG_SERIAL.print(freeRamBytes);
  DEBUG_SERIAL.print(F(" framesOk="));
  DEBUG_SERIAL.print(framesRxOk);
  DEBUG_SERIAL.print(F(" framesBad="));
  DEBUG_SERIAL.print(framesRxBad);
  DEBUG_SERIAL.print(F(" rawBytesESP="));
  DEBUG_SERIAL.print(totalRawBytesFromESP);
  DEBUG_SERIAL.print(F(" chkFail="));
  DEBUG_SERIAL.print(framesRxChkFail);
  DEBUG_SERIAL.print(F(" synced="));
  DEBUG_SERIAL.println(synced ? (syncDegraded ? F("SI(degradado)") : F("SI")) : F("NO"));

  // Reset de la ventana de medición: tanto las estadísticas de ciclo como
  // los contadores de frames (Ok/Bad/ChkFail) reflejan SOLO lo ocurrido
  // desde el heartbeat anterior, no un acumulado de toda la vida del
  // firmware que acaba saturando en 255 y dejando de aportar información.
  // El historial completo de errores individuales ya queda registrado en
  // el log de pantalla en el momento en que ocurren (ver
  // tryReadFrameFromESP y el dispatch de loop()).
  cycleSumUs = 0;
  cycleSampleCount = 0;
  cycleMaxUs = 0;
  framesRxOk = 0;
  framesRxBad = 0;
  framesRxChkFail = 0;
}

// ---------------------------------------------------------------------
// LED de estado: parpadeo de código (N veces + pausa larga), no bloqueante
// ---------------------------------------------------------------------
bool watchdogRecoveredAtBoot = false; // se fija una vez en setup(), no cambia

#define BLINK_ON_MS 150
#define BLINK_OFF_MS 150
#define BLINK_PAUSE_MS 1200

uint8_t currentStatusCode() {
  if (watchdogRecoveredAtBoot) return STATUS_WATCHDOG_RECOVERED;
  if (!synced) return STATUS_SYNCING; // handshake inicial con el ESP en curso
  if (framesRxBad > 0 || framesRxChkFail > 0) return STATUS_BAD_FRAMES; // errores EN ESTA VENTANA (desde el último heartbeat)
  if (!everReceivedGoodZ21Frame) return STATUS_NO_FRAMES_EVER; // nunca, en toda la vida del firmware, ni un solo frame válido
  return STATUS_OK;
}

void updateStatusLed() {
  static unsigned long lastChangeMs = 0;
  static uint8_t blinksLeft = 0;
  static bool ledOn = false;

  unsigned long nowMs = millis();
  uint8_t code = currentStatusCode();

  if (blinksLeft == 0 && !ledOn && (nowMs - lastChangeMs) >= BLINK_PAUSE_MS) {
    // arrancar una nueva ronda de N parpadeos
    blinksLeft = code;
    ledOn = true;
    digitalWrite(LED_PIN, HIGH);
    lastChangeMs = nowMs;
    return;
  }

  if (ledOn && (nowMs - lastChangeMs) >= BLINK_ON_MS) {
    ledOn = false;
    digitalWrite(LED_PIN, LOW);
    lastChangeMs = nowMs;
    blinksLeft--;
    return;
  }

  if (!ledOn && blinksLeft > 0 && (nowMs - lastChangeMs) >= BLINK_OFF_MS) {
    ledOn = true;
    digitalWrite(LED_PIN, HIGH);
    lastChangeMs = nowMs;
    return;
  }
}

// ---------------------------------------------------------------------
// Pantalla: construcción del snapshot de estado (ver display_types.h)
// ---------------------------------------------------------------------
// El módulo de pantalla no lee ninguna variable global de este sketch
// directamente (ver DECISIÓN DE DISEÑO en display_types.h) — aquí se
// arma la "foto" a partir de las variables que YA existen para el resto
// del firmware (heartbeat, LED de estado, sincronización...), sin
// duplicar ningún cálculo.
DisplayStatusSnapshot buildDisplaySnapshot() {
  DisplayStatusSnapshot snap;
  snap.fwVersionMajor = MEGA_FW_VERSION_MAJOR;
  snap.fwVersionMinor = MEGA_FW_VERSION_MINOR;
  snap.uptimeMs = millis();
  snap.synced = synced;
  snap.syncDegraded = syncDegraded;
  snap.netInfoMode = netInfoMode;
  snap.netInfoIp = netInfoIp;
  snap.netInfoMac = netInfoMac;
  snap.netInfoSsid = netInfoSsid;
  snap.statusCode = currentStatusCode();
  snap.watchdogRecoveredAtBoot = watchdogRecoveredAtBoot;
  snap.framesRxOk = framesRxOk;
  snap.framesRxBad = framesRxBad;
  snap.framesRxChkFail = framesRxChkFail;
  snap.freeRamBytes = (uint16_t)freeRam();
  snap.locoValid = hasLastLocoState;
  if (hasLastLocoState) {
    snap.locoAddress = lastLocoState.address;
    snap.locoStepsCode = lastLocoState.stepsCode;
    snap.locoSpeedByte = lastLocoState.speedByte;
    snap.locoForward = (lastLocoState.speedByte & 0x80) != 0;
    snap.locoF0 = (lastLocoState.f0to4 >> 4) & 0x01;
  } else {
    snap.locoAddress = 0;
    snap.locoStepsCode = 4;
    snap.locoSpeedByte = 0x80;
    snap.locoForward = true;
    snap.locoF0 = 0;
  }
  return snap;
}

// ---------------------------------------------------------------------
// Dispatch principal
// ---------------------------------------------------------------------
void handleDataset(const uint8_t *payload, uint8_t len) {
  if (len < 4) {
    displayLogf("Dataset Z21 invalido (len=%u)", (unsigned)len);
    return; // dataset inválido, se ignora
  }

  uint16_t header = payload[2] | (payload[3] << 8);
  const uint8_t *data = payload + 4;
  uint8_t dataLen = len - 4;

  switch (header) {
    case LAN_CAN_DETECTOR:
      handleCanDetector(data, dataLen);
      break;
    case LAN_GET_SERIAL_NUMBER:
      handleGetSerialNumber();
      break;
    case LAN_GET_HWINFO:
      handleGetHwInfo();
      break;
    case LAN_GET_CODE:
      handleGetCode();
      break;
    case LAN_LOGOFF:
      // Sin respuesta, ver spec
      break;
    case LAN_SET_BROADCASTFLAGS:
      handleSetBroadcastFlags(data, dataLen);
      break;
    case LAN_GET_BROADCASTFLAGS:
      handleGetBroadcastFlags();
      break;
    case LAN_SYSTEMSTATE_GETDATA:
      handleSystemStateGetData();
      break;
    case LAN_GET_COMMUNICATION_INFO:
      handleGetCommunicationInfo();
      break;
    case 0x40: // LAN_X — hay que mirar el XHeader (data[0]) para saber cuál es
      if (dataLen < 1) break;
      switch (data[0]) {
        case 0x21: // LAN_X_GET_VERSION / LAN_X_GET_STATUS / TRACK_POWER comparten XHeader 0x21
          if (dataLen >= 2 && data[1] == 0x21) {
            handleXGetVersion();
          } else if (dataLen >= 2 && data[1] == 0x24) {
            handleXGetStatus();
          } else if (dataLen >= 2 && data[1] == 0x80) {
            // Verificado contra el PDF (sección 2.5): XHeader 0x21, DB0 0x80
            handleXSetTrackPowerOff();
          } else if (dataLen >= 2 && data[1] == 0x81) {
            // Verificado contra el PDF (sección 2.6): XHeader 0x21, DB0 0x81
            handleXSetTrackPowerOn();
          }
          break;
        case 0xE3: // LAN_X_GET_LOCO_INFO
          handleXGetLocoInfo(data, dataLen);
          break;
        case 0xF1: // LAN_X_GET_FIRMWARE_VERSION (PDF sección 2.15)
          if (dataLen >= 2 && data[1] == 0x0A) {
            handleXGetFirmwareVersion();
          }
          break;
        case 0xE4: // LAN_X_SET_LOCO_DRIVE / LAN_X_SET_LOCO_FUNCTION
          handleXSetLocoDriveOrFunction(data, dataLen);
          break;
        case 0x43: // LAN_X_GET_TURNOUT_INFO (PDF sección 5.1)
          handleXGetTurnoutInfo(data, dataLen);
          break;
        case 0x53: // LAN_X_SET_TURNOUT (PDF sección 5.2)
          handleXSetTurnout(data, dataLen);
          break;
        case 0x80:
          // Verificado contra el PDF (sección 2.13): LAN_X_SET_STOP tiene
          // XHeader 0x80 y NO lleva DB0 (solo XHeader+XOR, DataLen 0x06).
          handleXSetStop();
          break;
        default:
          // Comando LAN_X no reconocido todavía en esta versión dummy
          // (TODO: ampliar cobertura conforme avance la sección 3 del spec).
          break;
      }
      break;
    default:
      // Header no reconocido todavía en esta versión dummy.
      break;
  }
}

// ---------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------
void setup() {
  // Si el reset anterior fue provocado por el watchdog, MCUSR lo indica
  // (bit WDRF). Se guarda antes de limpiar el registro, para que el LED
  // pueda mostrar el código 4 (STATUS_WATCHDOG_RECOVERED) hasta el
  // siguiente arranque manual — es una señal de que el firmware se colgó
  // en algún momento y conviene revisarlo, no algo a ignorar.
  watchdogRecoveredAtBoot = (MCUSR & (1 << WDRF)) != 0;
  MCUSR = 0;
  wdt_disable(); // por si venía habilitado de un reset anterior

  megaSerialNumber = loadOrInitSerialNumber();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  DEBUG_SERIAL.begin(DEBUG_BAUD);
  DEBUG_SERIAL.println();
  DEBUG_SERIAL.print(F("=== mega_z21 firmware v"));
  DEBUG_SERIAL.print(MEGA_FW_VERSION_MAJOR);
  DEBUG_SERIAL.print(F("."));
  DEBUG_SERIAL.print(MEGA_FW_VERSION_MINOR);
  DEBUG_SERIAL.println(F(" ==="));
  if (watchdogRecoveredAtBoot) {
    DEBUG_SERIAL.println(F("!!! Arranque por RESET DEL WATCHDOG (el firmware se colgo) !!!"));
  }
  DEBUG_SERIAL.println(F("Esperando frames del ESP8266 por Serial3..."));

  LINK_SERIAL.begin(LINK_BAUD);

  // Pantalla 3.5" TFT: estado del sistema + log de comunicación. Ver
  // AGENT.md ("Pantalla 3.5 TFT") y docs/Z21_EMULATOR_SPEC.md (sección
  // 9). Sin encoder todavía, así que es una única vista fija — el
  // objetivo de esta primera versión es poder ver a simple vista si el
  // gateway está listo y a qué se ha conectado, sin cable USB.
  displayInit();
  displayLogf("mega_z21 fw v%u.%u", MEGA_FW_VERSION_MAJOR, MEGA_FW_VERSION_MINOR);
  displayLogf("Serial: %u", (unsigned)megaSerialNumber);
  if (watchdogRecoveredAtBoot) {
    displayLogF(F("!! Reinicio por WATCHDOG !!"));
  }

  // Backend de tracción activo (ver selector al principio del sketch).
  // El backend XpressNet inicializa aquí dentro su propio Serial1 (RS485)
  // vía la librería externa — el dummy no toca ningún puerto físico.
  traction.begin();
#if TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_XPRESSNET
  displayLogF(F("Backend traccion: XpressNet"));
#else
  displayLogF(F("Backend traccion: DUMMY (sin bus)"));
#endif

  // TODO: SD del shield (log de sesión + base de datos de locomotoras) y
  //       encoder rotativo — no usado todavía, ver display_types.h
  //       (DisplayScreenMode) para cómo se prevé encajar el encoder más
  //       adelante sin rehacer el módulo de pantalla.
  // TODO: inicializar interrupción del botón de parada de emergencia
  //       (crítico, ver AGENT.md, "Seguridad" — no debe faltar antes de
  //       probar con hardware real conectado a la vía).

  wdt_enable(WATCHDOG_TIMEOUT); // si loop() se cuelga, reset automático
}

void loop() {
  wdt_reset(); // "doy señales de vida" — si esto no se ejecuta en el
               // tiempo del timeout, el watchdog resetea el Mega solo

  trackCycleTime();
  traction.poll(); // no bloqueante: ver ITractionBackend::poll() en traction_backend.h
  sendHeartbeatIfDue();
  updateStatusLed();
  // NOTA DE FUSIÓN (histórico): durante la depuración del bug de framing
  // ESP<->Mega (maxLen uint8_t/uint16_t, ver tryReadFrameFromESP más
  // arriba) esta llamada estuvo comentada a propósito, para descartar el
  // coste de SPI/bus paralelo del refresco de cabecera mientras se
  // buscaba el bug real. Ya verificado en hardware que el fix de maxLen
  // restaura la comunicación, así que el refresco en vivo de la cabecera
  // está reactivado (línea de abajo) — no volver a comentarla sin motivo.
  displayTick(buildDisplaySnapshot());

  if (!synced) {
    runSyncStep(); // manda HELLO periódicamente hasta recibir NET_INFO
  }

  uint8_t type = 0;
  uint8_t len = 0;
  if (tryReadFrameFromESP(type, linkBuf, LINK_BUF_SIZE, len)) {
    DEBUG_SERIAL.print(F("[RX] type=0x"));
    DEBUG_SERIAL.print(type, HEX);
    DEBUG_SERIAL.print(F(" len="));
    DEBUG_SERIAL.print(len);
    DEBUG_SERIAL.print(F(" data="));
    for (uint8_t i = 0; i < len; i++) {
      if (linkBuf[i] < 0x10) DEBUG_SERIAL.print('0');
      DEBUG_SERIAL.print(linkBuf[i], HEX);
      DEBUG_SERIAL.print(' ');
    }
    DEBUG_SERIAL.println();

    switch (type) {
      case FRAME_TYPE_NET_INFO:
        // Puede llegar más de una vez (p.ej. el ESP se reconecta a otra
        // red); siempre la tramitamos, no solo la primera.
        handleNetInfo(linkBuf, len);
        break;

      case FRAME_TYPE_Z21:
        if (!synced) {
          // El ESP no debería mandar tráfico Z21 antes de completar el
          // handshake, pero por si acaso llega algo, se ignora sin
          // contarlo como error — todavía no hemos "levantado" el Z21.
          break;
        }
        if (len >= 4) {
          framesRxOk = (framesRxOk < 255) ? framesRxOk + 1 : 255;
          everReceivedGoodZ21Frame = true;
          handleDataset(linkBuf, len);
        } else {
          framesRxBad = (framesRxBad < 255) ? framesRxBad + 1 : 255;
          static unsigned long lastBadFrameLogMs = 0;
          if (displayLogThrottle(lastBadFrameLogMs, 500)) {
            displayLogf("Frame Z21 con len invalido (%u en esta ventana)", (unsigned)framesRxBad);
          }
        }
        break;

      default:
        // HELLO/SYNC_ACK son los que manda el propio Mega, no se esperan
        // de entrada; HEARTBEAT tampoco (solo va Mega->ESP). Cualquier
        // otro tipo se ignora silenciosamente.
        break;
    }
  }
}
