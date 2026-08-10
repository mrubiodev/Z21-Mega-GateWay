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
 * ENTRADAS FÍSICAS (input/input_config.h, input/encoder_input.h/.cpp):
 * parada de emergencia por interrupción hardware y encoder rotativo con
 * pulsador. NINGUNO de los dos está montado en el hardware actual — el
 * código está completo y listo, pero solo se activa de verdad
 * (pinMode/attachInterrupt) si su flag correspondiente
 * (EMERGENCY_STOP_HARDWARE_PRESENT / ENCODER_HARDWARE_PRESENT, ambos en
 * input_config.h) está en 1. La parada de emergencia usa una ISR mínima
 * (solo pone un flag `volatile`) más una comprobación inmediata al
 * principio de loop() — ver el razonamiento de por qué esto SÍ cumple
 * "sin cola que pueda bloquearse" (AGENT.md, "Seguridad") en
 * input_config.h. El encoder, de momento, solo vuelca sus eventos al log
 * de pantalla (no hay menú real todavía, ver SCREEN_MODE_LOCO/CONFIG en
 * display_types.h).
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
#include "input/input_config.h" // pines/flags de parada de emergencia y encoder — ver ese
                                 // fichero para el porqué de cada uno antes de tocar nada aquí
#include "input/encoder_input.h" // encoder rotativo + pulsador (ver input_config.h,
                                  // ENCODER_HARDWARE_PRESENT — API no-op si no está montado)

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

// ---------------------------------------------------------------------
// Multi-cliente Z21 (v0.11) — ver el comentario grande junto a
// Z21_MAX_CLIENTS/CLIENT_ID_NONE/BCFLAG_* en z21_protocol.h para el
// diseño completo del client-id en el framing.
// ---------------------------------------------------------------------
// A qué cliente responder AHORA MISMO: se fija justo antes de
// handleDataset() (ver loop()) con el client-id que venía en el frame del
// ESP, y se vuelve a CLIENT_ID_NONE en cuanto termina — así cualquier cosa
// que se dispare FUERA de atender una petición de red concreta (hoy, solo
// el botón físico de e-stop) no se confunde con la última petición
// atendida. sendDataset()/sendXDataset() (más abajo) siempre contestan a
// currentReplyClientId.
uint8_t currentReplyClientId = CLIENT_ID_NONE;

// Cliente que inició la última LAN_X_CV_READ/WRITE en curso (PDF 6.4/6.5:
// LAN_X_CV_NACK/LAN_X_CV_RESULT se mandan "al cliente que inició la
// programación", como respuesta directa, no como broadcast). Solo puede
// haber una programación de CV en curso a la vez (mismo límite que
// cvPending_ dentro del backend XpressNet, que además ahora RECHAZA
// explícitamente una segunda petición mientras la primera sigue
// pendiente — ver cvRead()/cvWrite() en traction_backend_xpressnet.cpp —
// para no sobreescribir este valor y dejar al primer cliente huérfano,
// sin NACK y sin resultado).
uint8_t pendingCvClientId = CLIENT_ID_NONE;

// Flags de LAN_SET_BROADCASTFLAGS, uno por slot de cliente (mismo índice
// que z21Clients[] en el ESP — ver AGENT.md, "el ESP no debe interpretar
// contenido Z21": el Mega es quien decide a quién le interesa cada
// broadcast, el ESP solo enruta por client-id). Todo a 0 al arrancar =
// ningún cliente suscrito a nada todavía, coherente con la Z21 real.
uint32_t clientBroadcastFlags[Z21_MAX_CLIENTS];

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

// ---------------------------------------------------------------------
// Parada de emergencia física (seta) — ver AGENT.md, sección "Seguridad"
// (no negociable) y el razonamiento completo en input_config.h.
// ---------------------------------------------------------------------
//
// La ISR NO llama a handleXSetStop() directamente. Hace lo mínimo posible
// (poner este flag a true) y deja que loop() actúe de inmediato en la
// siguiente vuelta — ver el porqué detallado en input_config.h, no es
// "pasar por una cola que puede bloquearse", es evitar Serial3.write()/SPI
// de la pantalla dentro de un contexto de interrupción.
volatile bool emergencyStopTriggered = false;
volatile unsigned long lastEmergencyStopIsrMs = 0;

void estopISR() {
  // Antirrebote mínimo dentro de la propia ISR: millis() es seguro de
  // leer aquí (es un contador global que ya se actualiza por su propio
  // timer interrupt, no hay I/O ni asignación de memoria implicada).
  unsigned long now = millis();
  if (now - lastEmergencyStopIsrMs < EMERGENCY_STOP_DEBOUNCE_MS) return;
  lastEmergencyStopIsrMs = now;
  emergencyStopTriggered = true;
}

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

// Construye y manda un dataset Z21 a UN cliente concreto (por su
// client-id/slot). Antepone el byte de CLIENT_ID que exige el framing
// desde v0.11 (ver z21_protocol.h) — el "DataLen" de 2 bytes que sigue es
// el del protocolo Z21 real y NO incluye ese byte, solo la cabecera+data.
//
// targetClientId == CLIENT_ID_NONE: no hay a quién responder de verdad
// (p.ej. el botón físico de e-stop no es respuesta a ninguna petición de
// red) — no se manda nada al ESP en vez de mandar un client-id inválido.
void sendDatasetToClient(uint8_t targetClientId, uint16_t header, const uint8_t *data, uint8_t dataLen) {
  if (targetClientId == CLIENT_ID_NONE) return;
  uint16_t total = 4 + dataLen; // DataLen Z21 incluye los 4 bytes de cabecera (header+len), no el client-id
  if (total > LINK_BUF_SIZE - 1) return; // no debería pasar con los datasets que maneja este firmware; guarda de seguridad
  respBuf[0] = targetClientId;
  respBuf[1] = total & 0xFF;
  respBuf[2] = (total >> 8) & 0xFF;
  respBuf[3] = header & 0xFF;
  respBuf[4] = (header >> 8) & 0xFF;
  for (uint8_t i = 0; i < dataLen; i++) respBuf[5 + i] = data[i];
  sendFrameToESP(FRAME_TYPE_Z21, respBuf, (uint8_t)(total + 1));
}

// Respuesta DIRECTA: al cliente que mandó el comando que se está
// atendiendo ahora mismo (currentReplyClientId, fijado en loop() justo
// antes de llamar a handleDataset()).
void sendDataset(uint16_t header, const uint8_t *data, uint8_t dataLen) {
  sendDatasetToClient(currentReplyClientId, header, data, dataLen);
}

// BROADCAST: el mismo dataset a todos los clientes conocidos que tengan
// el bit `flagBit` activo en su LAN_SET_BROADCASTFLAGS, salvo
// excludeClientId (normalmente currentReplyClientId, para no duplicar la
// respuesta directa que ya se mandó por sendDataset ahí donde aplica; pasar
// CLIENT_ID_NONE si no hay que excluir a nadie, p.ej. el e-stop físico).
//
// El Mega no sabe aquí qué slots tienen de verdad un cliente conectado en
// el ESP (esa tabla vive solo allí, ver AGENT.md) — simplemente se
// recorren los Z21_MAX_CLIENTS slots posibles; un slot sin cliente real
// nunca habrá tenido su flag activado (clientBroadcastFlags[i] == 0 por
// defecto), así que el bucle no le manda nada y es inofensivo.
void broadcastDataset(uint32_t flagBit, uint8_t excludeClientId, uint16_t header, const uint8_t *data, uint8_t dataLen) {
  for (uint8_t i = 0; i < Z21_MAX_CLIENTS; i++) {
    if (i == excludeClientId) continue;
    if ((clientBroadcastFlags[i] & flagBit) == 0) continue;
    sendDatasetToClient(i, header, data, dataLen);
  }
}

// Un dataset LAN_X va dentro de un dataset con Header=0x40; xData ya debe
// incluir el checksum final (XOR de XHeader..DBn).
void sendXDataset(const uint8_t *xData, uint8_t xLen) {
  sendDataset(0x40, xData, xLen);
}

void broadcastXDataset(uint32_t flagBit, uint8_t excludeClientId, const uint8_t *xData, uint8_t xLen) {
  broadcastDataset(flagBit, excludeClientId, 0x40, xData, xLen);
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

// Ver z21_protocol.h, payload "WifiAttempt" (FRAME_TYPE_WIFI_ATTEMPT).
// A diferencia de handleNetInfo() (que solo llega UNA vez resuelta la
// conexión y completado el handshake), este frame puede llegar mientras
// !synced -- de hecho es lo normal, ver el historial de z21_protocol.h --
// así que no depende para nada de `synced`. Vuelca el intento en el log
// de comunicación de la pantalla: SSID, estado e intento (N/total), NO la
// password -- el Mega nunca la recibe, por diseño (petición de usuario);
// para verla, el portal web del ESP la muestra (ver htmlConfigPage() en
// esp8266_wifi.ino). No se añade una fila nueva a la cabecera de estado
// (ya sin hueco libre, ver display_status_panel.cpp) -- el log es donde
// ya vivían el resto de datos de red que no caben ahí (SSID, gateway).
void handleWifiAttempt(const uint8_t *payload, uint8_t len) {
  if (len < 4) return; // frame corrupto/corto, se ignora (igual que handleNetInfo)

  uint8_t state = payload[0];
  uint8_t index = payload[1];
  uint8_t total = payload[2];

  const uint8_t ssidLenOffset = 3;
  uint8_t ssidLen = payload[ssidLenOffset];
  if (ssidLen > WIFI_ATTEMPT_SSID_MAXLEN) ssidLen = WIFI_ATTEMPT_SSID_MAXLEN;
  uint8_t ssidOffset = ssidLenOffset + 1;
  if ((uint16_t)(ssidOffset + ssidLen) > len) ssidLen = (len > ssidOffset) ? (len - ssidOffset) : 0;

  char ssid[WIFI_ATTEMPT_SSID_MAXLEN + 1];
  for (uint8_t i = 0; i < ssidLen; i++) ssid[i] = (char)payload[ssidOffset + i];
  ssid[ssidLen] = '\0';

  switch (state) {
    case WIFI_ATTEMPT_TRYING:
      displayLogf("WiFi %u/%u: probando %s", (unsigned)index, (unsigned)total, ssid);
      break;
    case WIFI_ATTEMPT_CONNECTED:
      displayLogf("WiFi %u/%u: CONECTADO a %s", (unsigned)index, (unsigned)total, ssid);
      break;
    case WIFI_ATTEMPT_FAILED:
      displayLogf("WiFi %u/%u: FALLO con %s", (unsigned)index, (unsigned)total, ssid);
      break;
    case WIFI_ATTEMPT_AP_FALLBACK:
      displayLogF(F("WiFi: sin redes disponibles, modo AP"));
      break;
    default:
      break; // estado desconocido: se ignora, igual que un FRAME_TYPE no reconocido
  }
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
// app. Desde v0.11 SÍ hay un almacén real de flags por cliente
// (clientBroadcastFlags[], ver más arriba) indexado por el mismo
// client-id/slot que usa el ESP en z21Clients[] — cada cliente guarda y
// lee su propio valor, ya no comparten uno global. Solo BCFLAG_BASIC y
// BCFLAG_SYSTEMSTATE (ver z21_protocol.h) provocan de verdad un envío en
// este firmware; el resto de bits del PDF se aceptan y se guardan tal
// cual (para que un GET posterior devuelva lo mismo que se mandó) pero no
// disparan nada todavía.
void handleSetBroadcastFlags(const uint8_t *data, uint8_t dataLen) {
  if (dataLen < 4) return;
  if (currentReplyClientId == CLIENT_ID_NONE) return; // no debería llegar aquí sin una petición de red detrás
  clientBroadcastFlags[currentReplyClientId] =
      (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
      ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

void handleGetBroadcastFlags() {
  uint32_t flags = (currentReplyClientId == CLIENT_ID_NONE) ? 0 : clientBroadcastFlags[currentReplyClientId];
  uint8_t data[4] = {
    (uint8_t)(flags & 0xFF), (uint8_t)((flags >> 8) & 0xFF),
    (uint8_t)((flags >> 16) & 0xFF), (uint8_t)((flags >> 24) & 0xFF)
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
void buildSystemStateData(uint8_t data[16]) {
  int16_t temperature;
  uint16_t supplyMv;
  uint16_t mainCurrentMa;

#if TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_DUMMY
  // Backend dummy: sin bus real detrás, así que no hay NINGÚN sensor de
  // corriente/voltaje/temperatura que leer. Se simula un pequeño jitter
  // "para que la app vea vía activa" y así poder probar la UI sin
  // hardware — ESTO ES A PROPÓSITO SOLO AQUÍ, ver el comentario de abajo
  // para el motivo de por qué NO se hace lo mismo con el backend real.
  TrackState ts = traction.getTrackState();
  uint16_t jitterSlow = (uint16_t)((millis() / 3000) % 5); // 0..4, cambia cada 3s
  temperature = 24 + (int16_t)jitterSlow;                  // 24..28 °C
  supplyMv = 18000 + (jitterSlow * 20);                     // 18000..18080 mV
  mainCurrentMa = 0;
  if (ts.powerOn && !ts.emergencyStop) {
    uint16_t jitterFast = (uint16_t)((millis() / 500) % 30);
    mainCurrentMa = 300 + jitterFast;
  }
#else
  // Backend XpressNet real: este Mega es un ESCLAVO en el bus (ver
  // traction_backend_xpressnet.h) — la corriente/voltaje/temperatura de
  // verdad las mide el booster de la MultiMaus, no nosotros, y la
  // librería Digital-MoBa/XpressNet (rol slave) no expone esos valores
  // en ningún callback. Antes de este fix se rellenaban con el mismo
  // jitter simulado que el backend dummy, lo cual es ENGAÑOSO en cuanto
  // hay hardware real de por medio (la app mostraría una corriente que
  // no tiene nada que ver con lo que de verdad pasa en la vía — podría
  // ocultar un problema real o alarmar sin motivo). Se manda 0 con
  // honestidad hasta que exista algún sensor propio en este Mega (p.ej.
  // un ACS712 en la salida) que sí pueda leerse de verdad. buildCentral
  // StateByte() de abajo ya consulta traction.getTrackState() por su
  // cuenta, así que no hace falta duplicar la llamada aquí.
  temperature = 0;
  supplyMv = 0;
  mainCurrentMa = 0;
#endif

  data[0] = mainCurrentMa & 0xFF; data[1] = (mainCurrentMa >> 8) & 0xFF;
  data[2] = 0x00; data[3] = 0x00;
  data[4] = mainCurrentMa & 0xFF; data[5] = (mainCurrentMa >> 8) & 0xFF;
  data[6] = temperature & 0xFF; data[7] = (temperature >> 8) & 0xFF;
  data[8] = supplyMv & 0xFF; data[9] = (supplyMv >> 8) & 0xFF;
  data[10] = supplyMv & 0xFF; data[11] = (supplyMv >> 8) & 0xFF;
  data[12] = buildCentralStateByte();
  data[13] = 0x00;
  data[14] = 0x00;
  data[15] = CAP_DCC | CAP_LOCO_CMDS | CAP_ACCESSORY_CMDS;
}

void handleSystemStateGetData() {
  uint8_t data[16];
  buildSystemStateData(data);
  sendDataset(LAN_SYSTEMSTATE_DATACHANGED, data, 16);
}

// PDF 2.18: "se reporta de forma asíncrona ... cuando el cliente activó
// el broadcast correspondiente (flag 0x00000100)". Antes de este fix
// NUNCA se mandaba nada por este flag salvo como respuesta directa a
// LAN_SYSTEMSTATE_GETDATA — un cliente suscrito solo con BCFLAG_
// SYSTEMSTATE (sin BCFLAG_BASIC) se quedaba sin ninguna notificación
// hasta que volviera a preguntar él mismo. Se llama desde
// onTractionChange() cada vez que cambia algo relevante para
// CentralState (power/e-stop/short/programming).
void broadcastSystemStateChanged() {
  uint8_t data[16];
  buildSystemStateData(data);
  broadcastDataset(BCFLAG_SYSTEMSTATE, CLIENT_ID_NONE, LAN_SYSTEMSTATE_DATACHANGED, data, 16);
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
// tiene un motivo objetivo para desconfiar de la central. V1.42 para ser
// COHERENTE con la versión ya declarada en handleGetHwInfo() (AGENT.md
// insiste en que todo el handshake debe responder de forma coherente
// entre sí) — antes decía V1.40 aquí y V1.42 en HWINFO, inconsistencia
// que ya existía y que además ahora importa de verdad: el byte extendido
// F29-F31 de LAN_X_LOCO_INFO (ver sendLocoInfoResponse) solo tiene
// sentido declarando FW>=1.42 (PDF sección 4.4).
void handleXGetFirmwareVersion() {
  uint8_t x[5];
  x[0] = 0xF3;
  x[1] = 0x0A;
  x[2] = 0x01; // V_MSB (BCD): "1"
  x[3] = 0x42; // V_LSB (BCD): "42" -> V1.42
  x[4] = xorChecksum(x, 4);
  sendXDataset(x, 5);
}

// ---------------------------------------------------------------------
// Comandos v1 de tracción y locomotoras: el núcleo Z21 solo traduce
// entre el formato de datagrama LAN_X y llamadas a `traction`
// (ITractionBackend) — qué backend sea de verdad (dummy o XpressNet) es
// invisible desde aquí, ver el selector al principio del sketch.
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Broadcasts reutilizables (PDF 2.7/2.8/2.9/2.10/2.14): factorizados en
// funciones aparte porque a partir de ahora se disparan desde DOS sitios
// distintos: cuando el comando lo pide la app Z21 (handleXSetTrackPower*/
// handleXSetStop) Y cuando lo detecta el backend de tracción viniendo del
// bus (onTractionChange() más abajo, PDF: "... or fue cambiado por algún
// dispositivo de entrada (multiMaus)"). Antes de este refactor, un cambio
// de estado iniciado por la MultiMaus (o un corto real) nunca llegaba a
// los clientes Z21 conectados por WiFi — solo se notificaba cuando el
// comando venía de la propia app.
// ---------------------------------------------------------------------
void broadcastTrackPowerOff() {
  uint8_t x[3] = { 0x61, 0x00, 0x00 };
  x[2] = xorChecksum(x, 2);
  broadcastXDataset(BCFLAG_BASIC, CLIENT_ID_NONE, x, 3);
  displayLogF(F("Track power OFF"));
}

void broadcastTrackPowerOn() {
  uint8_t x[3] = { 0x61, 0x01, 0x00 };
  x[2] = xorChecksum(x, 2);
  broadcastXDataset(BCFLAG_BASIC, CLIENT_ID_NONE, x, 3);
  displayLogF(F("Track power ON"));
}

void broadcastStopped() {
  uint8_t x[3] = { 0x81, 0x00, 0x00 };
  x[2] = xorChecksum(x, 2);
  broadcastXDataset(BCFLAG_BASIC, CLIENT_ID_NONE, x, 3);
  displayLogF(F("*** PARADA DE EMERGENCIA ***"));
}

// LAN_X_BC_TRACK_SHORT_CIRCUIT (PDF 2.10) — X-Header 0x61, DB0 0x08. NO
// existía ningún 'case' ni ninguna llamada a esto en todo el sketch: un
// cortocircuito real en la vía se reflejaba en buildCentralStateByte()
// (así que LAN_X_GET_STATUS/SYSTEMSTATE lo mostrarían si se preguntaba),
// pero nunca se avisaba de forma proactiva a los clientes suscritos.
void broadcastTrackShortCircuit() {
  uint8_t x[3] = { 0x61, 0x08, 0x00 };
  x[2] = xorChecksum(x, 2);
  broadcastXDataset(BCFLAG_BASIC, CLIENT_ID_NONE, x, 3);
  displayLogF(F("*** CORTOCIRCUITO ***"));
}

// LAN_X_BC_PROGRAMMING_MODE (PDF 2.9) — X-Header 0x61, DB0 0x02. Tampoco
// existía: se manda cuando se entra en modo de programación de CVs,
// disparado tanto por nuestras propias LAN_X_CV_READ/WRITE (más abajo)
// como por si la MultiMaus entra en Service Mode por su cuenta.
void broadcastProgrammingMode() {
  uint8_t x[3] = { 0x61, 0x02, 0x00 };
  x[2] = xorChecksum(x, 2);
  broadcastXDataset(BCFLAG_BASIC, CLIENT_ID_NONE, x, 3);
  displayLogF(F("Modo de programacion CV activo"));
}

void handleXSetTrackPowerOff() {
  traction.setTrackPower(false);
  broadcastTrackPowerOff();
}

void handleXSetTrackPowerOn() {
  // Verificado contra el PDF (sección 2.6): este comando también termina
  // la parada de emergencia y el modo de programación si estuvieran
  // activos — cada backend concreto de ITractionBackend::setTrackPower
  // replica esa misma regla (ver traction_backend_dummy.h /
  // traction_backend_xpressnet.cpp).
  traction.setTrackPower(true);
  broadcastTrackPowerOn();
}

// LAN_X_CV_RESULT / LAN_X_CV_NACK (PDF 6.4/6.5): a diferencia de los
// broadcasts de arriba, esto es una respuesta DIRECTA al cliente que
// inició la programación (pendingCvClientId, fijado en el 'case' de
// LAN_X_CV_READ/WRITE del dispatcher) — el PDF es explícito: "is
// automatically sent to the client that initiated the programming".
void sendCvResult(uint16_t cvAddress, uint8_t value) {
  if (pendingCvClientId == CLIENT_ID_NONE) return;
  uint8_t x[6] = { 0x64, 0x14, (uint8_t)((cvAddress >> 8) & 0xFF), (uint8_t)(cvAddress & 0xFF), value, 0x00 };
  x[5] = xorChecksum(x, 5);
  sendDatasetToClient(pendingCvClientId, 0x40, x, 6);
  pendingCvClientId = CLIENT_ID_NONE;
}

void sendCvNack() {
  if (pendingCvClientId == CLIENT_ID_NONE) return;
  uint8_t x[3] = { 0x61, 0x13, 0x00 };
  x[2] = xorChecksum(x, 2);
  sendDatasetToClient(pendingCvClientId, 0x40, x, 3);
  pendingCvClientId = CLIENT_ID_NONE;
}

// Único punto de entrada de TODOS los cambios que un backend asíncrono
// (XpressNet) detecta viniendo del bus físico — registrado en setup()
// vía traction.setChangeCallback(). Ver TractionChangeEvent en
// traction_backend.h para el porqué de este mecanismo: antes de que
// existiera, ningún cambio iniciado por la MultiMaus (u otro cliente
// XpressNet) llegaba nunca a los clientes Z21 conectados por WiFi.
void onTractionChange(const TractionChangeEvent &ev) {
  switch (ev.type) {
    case TractionEventType::LocoChanged: {
      const LocoState *loco = traction.getLocoState(ev.address);
      if (loco != nullptr) {
        uint8_t adrMsbRaw = (uint8_t)((ev.address >> 8) & 0x3F);
        uint8_t adrLsb = (uint8_t)(ev.address & 0xFF);
        // CLIENT_ID_NONE: no excluye a nadie. Si este cambio es la
        // confirmación (por el bus) de un LAN_X_SET_LOCO_DRIVE que
        // mandó justo la app, esta es la corrección al estado
        // "todavía viejo" que ya se le había respondido de inmediato
        // (ver handleXSetLocoDriveOrFunction) — mientras que si viene
        // de la MultiMaus, es la ÚNICA notificación que recibirá.
        broadcastLocoInfo(adrMsbRaw, adrLsb, loco, CLIENT_ID_NONE);
      }
      break;
    }
    case TractionEventType::TurnoutChanged: {
      const AccessoryState *acc = traction.getTurnoutState(ev.address);
      if (acc != nullptr) {
        uint8_t adrMsb = (uint8_t)((ev.address >> 8) & 0xFF);
        uint8_t adrLsb = (uint8_t)(ev.address & 0xFF);
        broadcastTurnoutInfo(adrMsb, adrLsb, acc, CLIENT_ID_NONE);
      }
      break;
    }
    case TractionEventType::TrackPowerOn:
      broadcastTrackPowerOn();
      broadcastSystemStateChanged();
      break;
    case TractionEventType::TrackPowerOff:
      broadcastTrackPowerOff();
      broadcastSystemStateChanged();
      break;
    case TractionEventType::EmergencyStop:
      broadcastStopped();
      broadcastSystemStateChanged();
      break;
    case TractionEventType::ShortCircuit:
      broadcastTrackShortCircuit();
      broadcastSystemStateChanged();
      break;
    case TractionEventType::ProgrammingMode:
      broadcastProgrammingMode();
      broadcastSystemStateChanged();
      break;
    case TractionEventType::CvResult:
      sendCvResult(ev.address, ev.value);
      break;
    case TractionEventType::CvNack:
      sendCvNack();
      break;
  }
}

uint8_t buildLocoInfoXData(uint8_t x[11], uint8_t adrMsbRaw, uint8_t adrLsb, const LocoState *loco) {
  // Formato extendido (PDF 4.4): "Ab Z21 FW Version 1.42 ist DataLen >= 15
  // (n >= 8), zur Übertragung des Status von F29, F30 und F31" — un
  // noveno byte de datos (DB8) con F29 en el bit0, F30 en el bit1, F31 en
  // el bit2 (resto reservado/0). Es justo lo que guarda el bit0-2 de
  // loco->f29to36 (mismo orden que la tabla del grupo 6, ver
  // traction_types.h), así que basta con enmascarar. Es SOLO F29-F31: el
  // propio PDF (remark D de la tabla 4.3.2) dice que F32 en adelante NO
  // llevan confirmación de vuelta al cliente LAN ni en una Z21 real, así
  // que f37to44 en adelante NUNCA se mandan aquí aunque LocoState los
  // guarde para uso interno.
  x[0] = 0xEF; // X-Header LAN_X_LOCO_INFO
  x[1] = adrMsbRaw; // la app ignora los 2 bits altos, se devuelve tal cual se pidió
  x[2] = adrLsb;
  x[3] = loco->stepsCode;
  x[4] = loco->speedByte;
  x[5] = loco->f0to4;
  x[6] = loco->f5to12;
  x[7] = loco->f13to20;
  x[8] = loco->f21to28;
  x[9] = loco->f29to36 & 0x07; // DB8: solo bits F29-F31, resto a 0
  x[10] = xorChecksum(x, 10);
  return 11;
}

void sendLocoInfoResponse(uint8_t adrMsbRaw, uint8_t adrLsb, const LocoState *loco) {
  uint8_t x[11];
  buildLocoInfoXData(x, adrMsbRaw, adrLsb, loco);
  sendXDataset(x, 11);
}

// Igual que sendLocoInfoResponse, pero para el resto de clientes
// suscritos (BCFLAG_BASIC) tras un cambio real de la loco (SET_LOCO_DRIVE/
// FUNCTION/E_STOP) — PDF 4.4/4.5: "a los clientes suscritos". NO se llama
// desde handleXGetLocoInfo (una consulta no cambia nada, no hay nada que
// notificar a nadie más).
void broadcastLocoInfo(uint8_t adrMsbRaw, uint8_t adrLsb, const LocoState *loco, uint8_t excludeClientId) {
  uint8_t x[11];
  buildLocoInfoXData(x, adrMsbRaw, adrLsb, loco);
  broadcastXDataset(BCFLAG_BASIC, excludeClientId, x, 11);
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
  } else if (db0 == 0x29 && dataLen >= 5) {
    // Grupo 6 (F29-F36, PDF 4.3.2, ampliación Z21 FW V1.42). Con backend
    // XpressNet esto SOLO actualiza el estado en RAM (ver punto 6 de
    // "ASUNCIONES A VALIDAR" en traction_backend_xpressnet.h) — no hay
    // forma de mandarlo por esta librería/X-Bus clásico hacia la
    // MultiMaus. Se acepta y responde igual, coherente con el propio PDF
    // (remark D: incluso una Z21 real no confirma F32+ al cliente LAN).
    traction.setLocoFunctionGroup(addr, FunctionGroup::F29toF36, data[4]);
    displayLogf("Loco %u: F29-F36 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  } else if (db0 == 0x2A && dataLen >= 5) {
    traction.setLocoFunctionGroup(addr, FunctionGroup::F37toF44, data[4]); // grupo 7
    displayLogf("Loco %u: F37-F44 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  } else if (db0 == 0x2B && dataLen >= 5) {
    traction.setLocoFunctionGroup(addr, FunctionGroup::F45toF52, data[4]); // grupo 8
    displayLogf("Loco %u: F45-F52 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  } else if (db0 == 0x50 && dataLen >= 5) {
    traction.setLocoFunctionGroup(addr, FunctionGroup::F53toF60, data[4]); // grupo 9
    displayLogf("Loco %u: F53-F60 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  } else if (db0 == 0x51 && dataLen >= 5) {
    traction.setLocoFunctionGroup(addr, FunctionGroup::F61toF68, data[4]); // grupo 10
    displayLogf("Loco %u: F61-F68 = 0x%02X", (unsigned)addr, (unsigned)data[4]);
  }
  // LAN_X_SET_LOCO_BINARY_STATE (PDF 4.3.3, XHeader 0xE5) sigue sin
  // implementar: no comparte XHeader con este comando (0xE4), así que no
  // hace falta tocar esta función para él — haría falta un 'case' propio
  // en el dispatcher principal si se implementa más adelante.

  const LocoState *updatedLoco = traction.getLocoState(addr);
  if (updatedLoco != nullptr) {
    lastLocoState = *updatedLoco;
    hasLastLocoState = true;
  }
  sendLocoInfoResponse(data[2], data[3], updatedLoco);
  if (updatedLoco != nullptr) {
    broadcastLocoInfo(data[2], data[3], updatedLoco, currentReplyClientId);
  }
}

void handleXSetLocoEStop(const uint8_t *data, uint8_t dataLen) {
  // Verificado contra el PDF (sección 4.5): la tabla de la petición tiene
  // una errata en la cabecera de columnas ("DB0 DB2" en vez de "DB0 DB1"),
  // pero los campos listados son inequívocos: XHeader(0x92)+Adr_MSB+
  // Adr_LSB+XOR, 4 bytes de Data en total (DataLen=0x08). Sin respuesta
  // estándar propia: el PDF remite a 4.4 LAN_X_LOCO_INFO "a los clientes
  // suscritos", igual que ya hace este fichero con SET_LOCO_DRIVE.
  if (dataLen < 3) return;
  uint16_t addr = ((uint16_t)(data[1] & 0x3F) << 8) | data[2];
  // El E-Stop es un valor de velocidad concreto (paso 1, "Notfall-Halt")
  // dentro del mismo DB3 que usa LAN_X_SET_LOCO_DRIVE, NO un campo
  // aparte — por eso se reutiliza setLocoDrive() en vez de añadir un
  // método nuevo a ITractionBackend, conservando el sentido de marcha y
  // el formato de pasos que ya tuviera la loco (el PDF no dice que el
  // E-Stop cambie ninguno de los dos).
  const LocoState *before = traction.getLocoState(addr);
  uint8_t stepsCode = (before != nullptr) ? before->stepsCode : 4; // 128 pasos por defecto
  uint8_t dirBit = (before != nullptr) ? (before->speedByte & 0x80) : 0x80; // adelante por defecto
  traction.setLocoDrive(addr, stepsCode, dirBit | 0x01); // 0x01 = paso de velocidad E-Stop
  displayLogf("Loco %u: E-STOP", (unsigned)addr);

  const LocoState *updated = traction.getLocoState(addr);
  if (updated != nullptr) {
    lastLocoState = *updated;
    hasLastLocoState = true;
  }
  sendLocoInfoResponse(data[1], data[2], updated);
  if (updated != nullptr) {
    broadcastLocoInfo(data[1], data[2], updated, currentReplyClientId);
  }
}

void handleXPurgeLoco(const uint8_t *data, uint8_t dataLen) {
  // Verificado contra el PDF (sección 4.6): comparte XHeader 0xE3 con
  // LAN_X_GET_LOCO_INFO (ver dispatcher, que distingue por DB0: 0xF0=GET,
  // 0x44=PURGE). Data: XHeader(0xE3)+DB0(0x44)+Adr_MSB+Adr_LSB+XOR, 5
  // bytes en total (DataLen=0x09).
  if (dataLen < 4) return;
  uint16_t addr = ((uint16_t)(data[2] & 0x3F) << 8) | data[3];
  traction.purgeLoco(addr);
  displayLogf("Loco %u: purgada", (unsigned)addr);
  // PDF: "There is no response to the caller and no notification to
  // other clients" — a diferencia de todos los demás handlers de este
  // fichero, aquí NO se manda ningún LAN_X_LOCO_INFO de vuelta.
  if (hasLastLocoState && lastLocoState.address == addr) {
    hasLastLocoState = false; // el snapshot de la cabecera también queda obsoleto
  }
}

// ---------------------------------------------------------------------
// Accesorios (agujas/señales/desacopladores/descarriladores biestables,
// PDF sección 5 "Switching") — ver AccessoryState en traction_types.h y
// las asunciones de conversión documentadas en traction_backend_
// xpressnet.h/.cpp.
// ---------------------------------------------------------------------
uint8_t buildTurnoutInfoXData(uint8_t x[5], uint8_t adrMsb, uint8_t adrLsb, const AccessoryState *acc) {
  // Verificado contra el PDF (sección 5.3): X-Header 0x43, DB0=AdrMSB,
  // DB1=AdrLSB, DB2=000000ZZ, XOR — 5 bytes en total.
  x[0] = 0x43; // X-Header LAN_X_TURNOUT_INFO
  x[1] = adrMsb;
  x[2] = adrLsb;
  x[3] = (uint8_t)acc->position & 0x03;
  x[4] = xorChecksum(x, 4);
  return 5;
}

void sendTurnoutInfoResponse(uint8_t adrMsb, uint8_t adrLsb, const AccessoryState *acc) {
  uint8_t x[5];
  buildTurnoutInfoXData(x, adrMsb, adrLsb, acc);
  sendXDataset(x, 5);
}

// Igual que sendTurnoutInfoResponse, pero para el resto de clientes
// suscritos (BCFLAG_BASIC) tras un SET_TURNOUT real — PDF 5.2: "No
// standard answer, 5.3 LAN_X_TURNOUT_INFO to subscribed clients".
// excludeClientId es normalmente currentReplyClientId, para no
// duplicarle al que mandó el comando la respuesta directa que ya recibió.
void broadcastTurnoutInfo(uint8_t adrMsb, uint8_t adrLsb, const AccessoryState *acc, uint8_t excludeClientId) {
  uint8_t x[5];
  buildTurnoutInfoXData(x, adrMsb, adrLsb, acc);
  broadcastXDataset(BCFLAG_BASIC, excludeClientId, x, 5);
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
  // LAN_X_TURNOUT_INFO to subscribed clients" — desde v0.11 sí se
  // distingue: respuesta directa al que mandó el comando (sendTurnoutInfoResponse)
  // + broadcast al resto de clientes suscritos vía BCFLAG_BASIC
  // (broadcastTurnoutInfo, ver handleSetBroadcastFlags).
  const AccessoryState *acc = traction.getTurnoutState(addr);
  sendTurnoutInfoResponse(data[1], data[2], acc);
  broadcastTurnoutInfo(data[1], data[2], acc, currentReplyClientId);
}

// ---------------------------------------------------------------------
// Accesorios EXTENDIDOS (señales de más de 2 aspectos, decodificadores
// DCCext según RCN-213 — PDF sección 5.4-5.6). Direccionamiento DISTINTO
// al de los turnouts normales de arriba: aquí 'addr' es la RawAddress de
// RCN-213 tal cual, no el FAdr con conversión a puerto/salida — ver
// ExtAccessoryState en traction_types.h para el detalle completo.
// ---------------------------------------------------------------------
uint8_t buildExtAccessoryInfoXData(uint8_t x[6], uint8_t adrMsb, uint8_t adrLsb, const ExtAccessoryState *ext) {
  // Verificado contra el PDF (sección 5.6): X-Header 0x44, DB0=Adr_MSB,
  // DB1=Adr_LSB, DB2=DDDDDDDD (estado), DB3=Status, XOR — 6 bytes.
  x[0] = 0x44; // X-Header LAN_X_EXT_ACCESSORY_INFO
  x[1] = adrMsb;
  x[2] = adrLsb;
  x[3] = ext->state;
  x[4] = ext->hasData ? EXT_ACCESSORY_STATUS_VALID : EXT_ACCESSORY_STATUS_UNKNOWN;
  x[5] = xorChecksum(x, 5);
  return 6;
}

void sendExtAccessoryInfoResponse(uint8_t adrMsb, uint8_t adrLsb, const ExtAccessoryState *ext) {
  uint8_t x[6];
  buildExtAccessoryInfoXData(x, adrMsb, adrLsb, ext);
  sendXDataset(x, 6);
}

// Igual que sendExtAccessoryInfoResponse, pero para el resto de clientes
// suscritos (BCFLAG_BASIC) tras un SET_EXT_ACCESSORY real — mismo
// razonamiento que broadcastTurnoutInfo, ver ahí.
void broadcastExtAccessoryInfo(uint8_t adrMsb, uint8_t adrLsb, const ExtAccessoryState *ext, uint8_t excludeClientId) {
  uint8_t x[6];
  buildExtAccessoryInfoXData(x, adrMsb, adrLsb, ext);
  broadcastXDataset(BCFLAG_BASIC, excludeClientId, x, 6);
}

void handleXGetExtAccessoryInfo(const uint8_t *data, uint8_t dataLen) {
  // Verificado contra el PDF (sección 5.5): XHeader(1)+DB0=Adr_MSB+
  // DB1=Adr_LSB+DB2=0x00(reservado), sin más datos. DB2 se ignora tal
  // cual dice el PDF ("reserved for future extensions").
  if (dataLen < 3) return;
  uint16_t rawAddr = ((uint16_t)data[1] << 8) | data[2];
  traction.requestExtAccessoryRefresh(rawAddr);
  const ExtAccessoryState *ext = traction.getExtAccessoryState(rawAddr);
  sendExtAccessoryInfoResponse(data[1], data[2], ext);
}

void handleXSetExtAccessory(const uint8_t *data, uint8_t dataLen) {
  // Verificado contra el PDF (sección 5.4): XHeader(1)+DB0=Adr_MSB+
  // DB1=Adr_LSB+DB2=DDDDDDDD(estado)+DB3=0x00(reservado), sin más datos.
  if (dataLen < 5) return;
  uint16_t rawAddr = ((uint16_t)data[1] << 8) | data[2];
  uint8_t state = data[3];
  // data[4] (DB3) es el byte reservado del PDF, siempre 0x00 hasta
  // nuevas versiones del protocolo — se ignora a propósito, igual que ya
  // se ignora el bit Q de LAN_X_SET_TURNOUT más arriba.
  traction.setExtAccessory(rawAddr, state);
  displayLogf("Acc.ext %u: estado %u", (unsigned)rawAddr, (unsigned)state);
  // PDF sección 5.4: "No standard answer, or 5.6 LAN_X_EXT_ACCESSORY_INFO
  // to subscribed clients" — desde v0.11, igual que handleXSetTurnout:
  // respuesta directa al que mandó el comando + broadcast al resto de
  // suscritos.
  const ExtAccessoryState *ext = traction.getExtAccessoryState(rawAddr);
  sendExtAccessoryInfoResponse(data[1], data[2], ext);
  broadcastExtAccessoryInfo(data[1], data[2], ext, currentReplyClientId);
}

// LAN_X_CV_READ / LAN_X_CV_WRITE (PDF sección 6.1/6.2) — a diferencia de
// otros pares get/set de este protocolo, NO comparten X-Header: READ es
// 0x23/DB0=0x11, WRITE es 0x24/DB0=0x12 (ver los dos 'case' en el
// dispatcher). Antes de este fix no existía NINGÚN 'case' para ninguno de
// los dos: la app se quedaba sin respuesta si intentaba programar un CV
// (caía silenciosamente en el 'default'). El resultado real (LAN_X_CV_
// RESULT/NACK) llega más tarde, de forma asíncrona, vía TractionEventType
// ::CvResult/CvNack -> onTractionChange() -> sendCvResult()/sendCvNack().
//
// OJO — orden importa aquí: pendingCvClientId se fija ANTES de llamar a
// traction.cvRead()/cvWrite(), no después. Se confirmó (leyendo el código
// fuente real de Digital-MoBa/XpressNet, no solo su cabecera) que
// writeCVMode() dispara notifyCVResult() de forma SÍNCRONA, dentro de la
// misma llamada, sin esperar ninguna confirmación real del bus (ver punto
// 7b de "ASUNCIONES A VALIDAR" en traction_backend_xpressnet.h, ya
// actualizado). Si pendingCvClientId se fijara después de la llamada,
// como en una versión anterior de este código, el resultado de un WRITE
// llegaría y se descartaría en silencio (sendCvResult() lo ignora si
// pendingCvClientId sigue siendo CLIENT_ID_NONE) antes de que hubiera
// nada que decir a quién mandárselo.
void handleXCvReadWrite(const uint8_t *data, uint8_t dataLen) {
  bool isRead = (dataLen >= 4 && data[0] == 0x23 && data[1] == 0x11);
  bool isWrite = (dataLen >= 5 && data[0] == 0x24 && data[1] == 0x12);
  if (!isRead && !isWrite) return; // p.ej. LAN_X_MM_WRITE_BYTE (mismo XHeader 0x24, DB0=0xFF) — no implementado
  uint16_t cvAddress = ((uint16_t)data[2] << 8) | data[3]; // PDF: CV Address = (CVAdr_MSB<<8)+CVAdr_LSB, 0=CV1
  pendingCvClientId = currentReplyClientId; // ANTES de la llamada, ver comentario de arriba
  bool accepted = isRead ? traction.cvRead(cvAddress) : traction.cvWrite(cvAddress, data[4]);
  if (accepted) {
    displayLogf("CV %u: %s...", (unsigned)(cvAddress + 1), isRead ? "leyendo" : "escribiendo");
  } else {
    // Backend sin soporte de CVs (dummy), CV fuera de rango para la
    // librería XpressNet (>255, ver traction_backend_xpressnet.h), o ya
    // hay otra programación de CV en curso (ver cvPending_ en el backend
    // XpressNet) — se avisa con NACK inmediato en vez de dejar a la app
    // esperando algo que nunca va a llegar. pendingCvClientId se limpia
    // aquí mismo (no hubo ninguna petición real en curso que pueda
    // "robarle" la respuesta a nadie).
    pendingCvClientId = CLIENT_ID_NONE;
    uint8_t x[3] = { 0x61, 0x13, 0x00 };
    x[2] = xorChecksum(x, 2);
    sendXDataset(x, 3);
  }
}

void handleXSetStop() {
  traction.emergencyStopAll();
  broadcastStopped();
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
  // Devuelve BroadcastFlags (4 bytes) + Puerto (2 bytes) — los flags del
  // cliente que pregunta, no un valor global (ver clientBroadcastFlags[]).
  uint32_t flags = (currentReplyClientId == CLIENT_ID_NONE) ? 0 : clientBroadcastFlags[currentReplyClientId];
  uint8_t data[8] = {
    (uint8_t)(flags & 0xFF),
    (uint8_t)((flags >> 8) & 0xFF),
    (uint8_t)((flags >> 16) & 0xFF),
    (uint8_t)((flags >> 24) & 0xFF),
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
      // Sin respuesta, ver spec. Sí limpiamos sus broadcast flags: un
      // cliente que se desconecta explícitamente no debería seguir
      // "recibiendo" (a nivel de a quién se le manda) broadcasts después
      // de irse, y además mitiga parcialmente la reasignación de slot por
      // LRU en el ESP (ver limitación documentada junto a Z21_MAX_CLIENTS
      // en z21_protocol.h) para el caso de una desconexión ordenada.
      if (currentReplyClientId != CLIENT_ID_NONE) {
        clientBroadcastFlags[currentReplyClientId] = 0;
      }
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
        case 0xE3: // LAN_X_GET_LOCO_INFO (DB0=0xF0) / LAN_X_PURGE_LOCO (DB0=0x44) — comparten XHeader
          if (dataLen >= 2 && data[1] == 0xF0) {
            handleXGetLocoInfo(data, dataLen);
          } else if (dataLen >= 2 && data[1] == 0x44) {
            handleXPurgeLoco(data, dataLen);
          }
          break;
        case 0x92: // LAN_X_SET_LOCO_E_STOP (PDF sección 4.5)
          handleXSetLocoEStop(data, dataLen);
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
        case 0x44: // LAN_X_GET_EXT_ACCESSORY_INFO (PDF sección 5.5)
          handleXGetExtAccessoryInfo(data, dataLen);
          break;
        case 0x54: // LAN_X_SET_EXT_ACCESSORY (PDF sección 5.4)
          handleXSetExtAccessory(data, dataLen);
          break;
        case 0x23: // LAN_X_CV_READ (DB0=0x11), PDF sección 6.1
          handleXCvReadWrite(data, dataLen);
          break;
        case 0x24: // LAN_X_CV_WRITE (DB0=0x12), PDF sección 6.2 — X-Header
                    // DISTINTO de LAN_X_CV_READ (verificado contra el PDF:
                    // a diferencia de otros pares get/set de este mismo
                    // protocolo, READ y WRITE NO comparten X-Header). Este
                    // mismo 0x24 también lo usa LAN_X_MM_WRITE_BYTE
                    // (DB0=0xFF, PDF 6.12) — no implementado, se ignora
                    // dentro de handleXCvReadWrite() si DB0 no es 0x12.
          handleXCvReadWrite(data, dataLen);
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
  // Registra el callback de "cambios que vienen del bus" (ver
  // TractionChangeEvent en traction_backend.h y onTractionChange() más
  // abajo) — no-op para el backend dummy (nunca lo invoca, ver su
  // implementación por defecto en ITractionBackend).
  traction.setChangeCallback(onTractionChange);
#if TRACTION_BACKEND_SELECTED == TRACTION_BACKEND_XPRESSNET
  displayLogF(F("Backend traccion: XpressNet"));
#else
  displayLogF(F("Backend traccion: DUMMY (sin bus)"));
#endif

  // TODO: SD del shield (log de sesión + base de datos de locomotoras) —
  //       sigue sin implementar, no usada todavía.

  // Parada de emergencia física: ver input_config.h para el razonamiento
  // completo (cableado NC fail-safe, antirrebote, por qué RISING). Solo
  // se activa de verdad si EMERGENCY_STOP_HARDWARE_PRESENT es 1 — con la
  // seta todavía sin conectar (caso actual), dejar esto en 0 evita que un
  // pin flotante dispare una parada falsa en cada arranque.
#if EMERGENCY_STOP_HARDWARE_PRESENT
  pinMode(EMERGENCY_STOP_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(EMERGENCY_STOP_PIN), estopISR, RISING);
  displayLogF(F("E-stop fisico: ACTIVO"));
#else
  displayLogF(F("E-stop fisico: no conectado (ver input_config.h)"));
#endif

  // Encoder rotativo + pulsador: no-op si ENCODER_HARDWARE_PRESENT es 0
  // en input_config.h (ver encoder_input.cpp) — se llama siempre igual,
  // así no hace falta ningún #if aquí.
  encoderInit();
#if ENCODER_HARDWARE_PRESENT
  displayLogF(F("Encoder: ACTIVO"));
#else
  displayLogF(F("Encoder: no montado (ver input_config.h)"));
#endif

  wdt_enable(WATCHDOG_TIMEOUT); // si loop() se cuelga, reset automático
}

void loop() {
  wdt_reset(); // "doy señales de vida" — si esto no se ejecuta en el
               // tiempo del timeout, el watchdog resetea el Mega solo

  // Parada de emergencia: lo PRIMERO que hace loop(), antes de tocar el
  // enlace con el ESP o la pantalla — ver el flag `volatile` y estopISR()
  // más arriba, y el razonamiento completo en input_config.h. loop() da
  // vueltas en microsegundos en este firmware, así que comprobar el flag
  // aquí es, en la práctica, "inmediato" sin los riesgos de actuar desde
  // dentro de la propia interrupción.
  if (emergencyStopTriggered) {
    noInterrupts();
    emergencyStopTriggered = false;
    interrupts();
    handleXSetStop(); // ya hace traction.emergencyStopAll() + notifica LAN_X_BC_STOPPED + log
    displayLogF(F("(disparada por boton fisico)"));
  }

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

  // Encoder: todavía no hay SCREEN_MODE_LOCO/CONFIG que consuma estos
  // eventos (ver display_types.h), así que de momento solo se vuelcan al
  // log de pantalla — sirve para confirmar que el cableado/sentido de
  // giro son correctos en cuanto se monte, sin depender de tener ya un
  // menú construido encima. No-op si ENCODER_HARDWARE_PRESENT es 0.
  long encoderDelta = 0;
  bool encoderButtonPressed = false;
  if (encoderPoll(encoderDelta, encoderButtonPressed)) {
    if (encoderDelta != 0) {
      displayLogf("Encoder: %ld", encoderDelta);
    }
    if (encoderButtonPressed) {
      displayLogF(F("Encoder: pulsado"));
    }
  }

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

      case FRAME_TYPE_WIFI_ATTEMPT:
        // Puede llegar varias veces por arranque (una por cada red
        // guardada que se prueba, más el resultado final) e incluso antes
        // de completar el handshake de sync -- ver handleWifiAttempt().
        handleWifiAttempt(linkBuf, len);
        break;

      case FRAME_TYPE_Z21:
        if (!synced) {
          // El ESP no debería mandar tráfico Z21 antes de completar el
          // handshake, pero por si acaso llega algo, se ignora sin
          // contarlo como error — todavía no hemos "levantado" el Z21.
          break;
        }
        // Desde v0.11 el payload lleva 1 byte de CLIENT_ID delante del
        // datagrama Z21 real (ver z21_protocol.h) — por eso el mínimo
        // válido sube de 4 a 5 bytes. handleDataset() sigue recibiendo
        // exactamente lo mismo que antes (el datagrama sin el client-id);
        // currentReplyClientId es la única forma en que el resto del
        // sketch se entera de a quién responder.
        if (len >= 5) {
          framesRxOk = (framesRxOk < 255) ? framesRxOk + 1 : 255;
          everReceivedGoodZ21Frame = true;
          currentReplyClientId = linkBuf[0];
          handleDataset(linkBuf + 1, len - 1);
          currentReplyClientId = CLIENT_ID_NONE; // fuera de handleDataset() no hay "el que preguntó"
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
