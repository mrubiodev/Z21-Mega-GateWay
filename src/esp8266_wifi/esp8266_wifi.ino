/*
 * esp8266_wifi.ino
 * -----------------
 * Sketch para el ESP8266 de la placa combo Mega+WiFi R3.
 * Ver docs/Z21_EMULATOR_SPEC.md y AGENT.md para contexto completo.
 *
 * IMPORTANTE sobre el Serial:
 *   El ESP8266 de esta placa combo solo tiene un UART hardware realmente
 *   utilizable (Serial), y ese es justo el que el DIP switch de la placa
 *   conecta a Serial3 del Mega. Por tanto NO se puede usar Serial para
 *   depuración por USB a la vez que para el enlace con el Mega — mientras
 *   el DIP switch esté en modo MCU<->ESP, Serial es exclusivamente el
 *   enlace con el Mega. Para depurar el ESP hay que cambiar el switch a
 *   modo USB<->ESP temporalmente.
 *
 * Librerías usadas (estándar del core ESP8266 para Arduino):
 *   ESP8266WiFi, WiFiUdp, ESP8266WebServer, EEPROM
 *
 * Sniffer del enlace Serial con el Mega: activo por defecto (/sniffer),
 * decodifica los frames capturados (tipo, longitud, checksum OK/FAIL,
 * payload) en vez de mostrar solo hex plano — el hex plano sigue
 * disponible en /sniffer?raw=1 para casos límite. Pendiente para más
 * adelante: volcado por WebSocket en vez de auto-refresh HTTP simple.
 *
 * Log de eventos en memoria (/log, ver evLogf() más abajo): funciona
 * igual en producción que en modo standalone, porque nunca escribe en
 * Serial salvo que ESP_DEBUG_STANDALONE esté a 1. Con el DIP en modo
 * MCU<->ESP y el Mega conectado, /log y /sniffer son la forma de depurar
 * — el monitor serie del ESP ya no hace falta para nada en ese modo.
 *
 * Sincronización inicial con el Mega (ver z21_protocol.h y AGENT.md):
 * hasta que el Mega manda HELLO y nosotros contestamos con NET_INFO
 * (modo STA/AP, IP, SSID) y recibimos su SYNC_ACK, NO se reenvía tráfico
 * UDP real hacia/desde el Mega — así evitamos que la app Z21 le llegue a
 * hablar a un Mega que todavía no ha terminado de arrancar.
 */

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>

// wifi_set_macaddr() y STATION_IF/SOFTAP_IF vienen del SDK de bajo nivel
// del core ESP8266, no de ESP8266WiFi.h -- hace falta este include aparte
// para poder fijar una MAC personalizada (ver "MAC personalizada" más abajo).
extern "C" {
#include <user_interface.h>
}

#include "z21_protocol.h"

// ---------------------------------------------------------------------
// MODO DE DEPURACIÓN STANDALONE
// ---------------------------------------------------------------------
// Con esto en 1, el ESP imprime por Serial todo lo que hace: WiFi, cada
// paquete UDP recibido, cada request web. Solo tiene sentido con el DIP
// switch en la fila "USB<->ESP8266 (communication)" de la tabla del
// fabricante (5 y 6 en ON, resto OFF) — en ese modo el Mega queda
// desconectado del enlace, así que las llamadas a sendToMega() no llegan
// a ningún sitio (inofensivo) y el Serial queda libre para logs de texto
// legibles en el monitor serie.
//
// En 0 (producción, DIP en modo MCU<->ESP con el Mega conectado): el
// Serial queda 100% reservado para el framing con el Mega, tal cual
// necesita el enlace real. NO se pierde visibilidad — evLogf() sigue
// registrando todo en el ring buffer en RAM sea cual sea este valor, y se
// consulta por /log y /sniffer sin tocar el Serial para nada. Por eso ya
// no hace falta el monitor serie ni en producción.
#define ESP_DEBUG_STANDALONE 0

#if ESP_DEBUG_STANDALONE
  #define DBG(x) Serial.print(x)
  #define DBGLN(x) Serial.println(x)
#else
  #define DBG(x)
  #define DBGLN(x)
#endif

// ---------------------------------------------------------------------
// LOG DE EVENTOS EN MEMORIA (para depurar por WEB sin necesitar el
// monitor serie) — esto es lo que de verdad hace falta en producción:
// con el DIP en modo MCU<->ESP el Serial está reservado para el enlace
// con el Mega y NO se puede usar para texto de depuración (ver nota de
// cabecera del archivo). evLogf() nunca toca el Serial en ese modo — solo
// guarda en un ring buffer en RAM que se sirve por /log con auto-refresh,
// así que funciona igual de bien en producción que en modo standalone.
// Si ESP_DEBUG_STANDALONE está activo, además se espeja por Serial (por
// comodidad mientras se depura con el DIP en modo USB<->ESP).
// ---------------------------------------------------------------------
// Niveles del log -- antes todo era texto plano sin distinguir "esto es
// informativo" de "esto es un problema real". Con nivel se puede filtrar
// en /log (?level=warn, ?level=error) y resaltar en color, para no tener
// que leer 40 lineas buscando el unico WARN/ERROR relevante.
#define LOG_LVL_INFO 0
#define LOG_LVL_WARN 1
#define LOG_LVL_ERROR 2

#define EVLOG_LINES 60
#define EVLOG_LINE_LEN 96
struct EvLogEntry {
  unsigned long ms;
  uint8_t level;
  char text[EVLOG_LINE_LEN];
};
EvLogEntry evLog[EVLOG_LINES];
uint8_t evLogCount = 0;   // entradas ocupadas (hasta EVLOG_LINES)
uint8_t evLogHead = 0;    // siguiente posicion a escribir (circular)
uint32_t evLogTotal = 0;  // total historico (aunque se hayan sobrescrito)
uint32_t evLogWarnTotal = 0;
uint32_t evLogErrorTotal = 0;

void evLogAdd(uint8_t level, const char *text) {
  strncpy(evLog[evLogHead].text, text, EVLOG_LINE_LEN - 1);
  evLog[evLogHead].text[EVLOG_LINE_LEN - 1] = '\0';
  evLog[evLogHead].ms = millis();
  evLog[evLogHead].level = level;
  evLogHead = (evLogHead + 1) % EVLOG_LINES;
  if (evLogCount < EVLOG_LINES) evLogCount++;
  evLogTotal++;
  if (level == LOG_LVL_WARN) evLogWarnTotal++;
  if (level == LOG_LVL_ERROR) evLogErrorTotal++;
}

// Devuelve el indice real en evLog[] para la entrada cronologica i-esima
// (i=0 -> la mas antigua todavia presente, i=evLogCount-1 -> la mas
// reciente). Mismo patron que ya se usaba para sniffLog.
uint8_t evLogChronoIndex(uint8_t i) {
  uint8_t start = (evLogCount < EVLOG_LINES) ? 0 : evLogHead;
  return (start + i) % EVLOG_LINES;
}

const char *evLogLevelName(uint8_t level) {
  switch (level) {
    case LOG_LVL_WARN: return "WARN";
    case LOG_LVL_ERROR: return "ERROR";
    default: return "INFO";
  }
}

char evLogFmtBuf[EVLOG_LINE_LEN];
void evLogVf(uint8_t level, const char *fmt, va_list args) {
  vsnprintf(evLogFmtBuf, EVLOG_LINE_LEN, fmt, args);
  evLogAdd(level, evLogFmtBuf);
#if ESP_DEBUG_STANDALONE
  Serial.println(evLogFmtBuf);
#endif
}

// evLogf() se mantiene tal cual para no tener que tocar cada punto de
// llamada existente -- registra a nivel INFO por defecto. Para avisos o
// errores reales, usar evLogfL(LOG_LVL_WARN/LOG_LVL_ERROR, ...).
void evLogf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  evLogVf(LOG_LVL_INFO, fmt, args);
  va_end(args);
}

void evLogfL(uint8_t level, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  evLogVf(level, fmt, args);
  va_end(args);
}

// Reinicia el ring buffer de eventos (usado por /log?clear=1) sin perder
// los contadores historicos totales (evLogTotal/Warn/Error) -- esos son
// "desde el arranque", no "desde el ultimo clear".
void evLogClear() {
  evLogCount = 0;
  evLogHead = 0;
}

// Throttle sencillo para eventos que podrian repetirse muy rapido (p.ej.
// fallos de checksum durante una rafaga de ruido electrico) y que si no
// se limitan, desplazarian del ring buffer todo lo demas en milisegundos.
bool evLogThrottle(unsigned long &lastMs, unsigned long minIntervalMs) {
  unsigned long now = millis();
  if (lastMs != 0 && (now - lastMs) < minIntervalMs) return false;
  lastMs = now;
  return true;
}

// ---------------------------------------------------------------------
// Configuración persistente (EEPROM)
// ---------------------------------------------------------------------
// Layout simple de EEPROM: [0]=magic, [1..32]=SSID, [33..96]=password,
// [97..128]=usuario web, [129..192]=password web
#define EEPROM_SIZE 256
#define EEPROM_MAGIC 0x5A // marca para saber si hay config guardada
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_SSID 1
#define EEPROM_ADDR_SSID_LEN 32
#define EEPROM_ADDR_PASS 33
#define EEPROM_ADDR_PASS_LEN 64
#define EEPROM_ADDR_WEBUSER 97
#define EEPROM_ADDR_WEBUSER_LEN 32
#define EEPROM_ADDR_WEBPASS 129
#define EEPROM_ADDR_WEBPASS_LEN 32
// MAC personalizada (v0.7): flag de "esta activa" + los 6 bytes de la MAC.
// Se aplica a STA y AP por igual con wifi_set_macaddr() antes de conectar
// (ver applyMac()). Si el flag es 0 (nada guardado o campo vaciado), NO se
// usa la MAC de fabrica del chip: se genera una MAC por defecto con el
// prefijo (OUI) 84:2B:BC, propio de nuestra red, y los 3 bytes finales
// derivados del chip ID (estable entre reinicios, distinto por placa).
// El usuario puede seguir fijando cualquier otra MAC (incluso con otro
// prefijo) desde el formulario /  -- este es solo el valor por defecto.
#define EEPROM_ADDR_MAC_FLAG 161
#define EEPROM_ADDR_MAC 162
#define EEPROM_ADDR_MAC_LEN 6
#define DEFAULT_MAC_OUI_0 0x84
#define DEFAULT_MAC_OUI_1 0x2B
#define DEFAULT_MAC_OUI_2 0xBC

char cfgSSID[EEPROM_ADDR_SSID_LEN + 1] = "";
char cfgPass[EEPROM_ADDR_PASS_LEN + 1] = "";
char cfgWebUser[EEPROM_ADDR_WEBUSER_LEN + 1] = "admin";
char cfgWebPass[EEPROM_ADDR_WEBPASS_LEN + 1] = "z21admin"; // TODO: forzar cambio en primer arranque
bool cfgMacCustomEnabled = false;
uint8_t cfgMacAddr[EEPROM_ADDR_MAC_LEN] = {0, 0, 0, 0, 0, 0};

// ---------------------------------------------------------------------
// Estado de red
// ---------------------------------------------------------------------
#define STA_CONNECT_TIMEOUT_MS 10000
#define AP_SSID_PREFIX "Z21_"
#define AP_FIXED_IP IPAddress(192, 168, 0, 111)
#define AP_GATEWAY IPAddress(192, 168, 0, 111)
#define AP_SUBNET IPAddress(255, 255, 255, 0)

bool isAPMode = false;
String apSSID = ""; // se rellena en startAPFallback(); usado también en NET_INFO

WiFiUDP z21Udp;
ESP8266WebServer webServer(80);

// Recuerda de qué IP/puerto vino el último datagrama UDP, para poder
// reenviar hacia el cliente correcto la respuesta que llegue del Mega.
IPAddress lastClientIP;
uint16_t lastClientPort = 0;

// ---------------------------------------------------------------------
// EEPROM helpers
// ---------------------------------------------------------------------
void readEEPROMString(int addr, char *dest, int maxLen) {
  for (int i = 0; i < maxLen; i++) {
    dest[i] = char(EEPROM.read(addr + i));
  }
  dest[maxLen] = '\0';
}

void writeEEPROMString(int addr, const char *src, int maxLen) {
  int len = strlen(src);
  for (int i = 0; i < maxLen; i++) {
    EEPROM.write(addr + i, i < len ? src[i] : 0);
  }
}

void readEEPROMBytes(int addr, uint8_t *dest, int len) {
  for (int i = 0; i < len; i++) dest[i] = EEPROM.read(addr + i);
}

void writeEEPROMBytes(int addr, const uint8_t *src, int len) {
  for (int i = 0; i < len; i++) EEPROM.write(addr + i, src[i]);
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t magic = EEPROM.read(EEPROM_ADDR_MAGIC);
  if (magic == EEPROM_MAGIC) {
    readEEPROMString(EEPROM_ADDR_SSID, cfgSSID, EEPROM_ADDR_SSID_LEN);
    readEEPROMString(EEPROM_ADDR_PASS, cfgPass, EEPROM_ADDR_PASS_LEN);
    readEEPROMString(EEPROM_ADDR_WEBUSER, cfgWebUser, EEPROM_ADDR_WEBUSER_LEN);
    readEEPROMString(EEPROM_ADDR_WEBPASS, cfgWebPass, EEPROM_ADDR_WEBPASS_LEN);
    cfgMacCustomEnabled = (EEPROM.read(EEPROM_ADDR_MAC_FLAG) == EEPROM_MAGIC);
    if (cfgMacCustomEnabled) {
      readEEPROMBytes(EEPROM_ADDR_MAC, cfgMacAddr, EEPROM_ADDR_MAC_LEN);
    }
  }
  // Si no hay magic, se quedan los valores por defecto declarados arriba
  // (sin SSID guardado -> irá directo a modo AP, sin MAC personalizada).
}

void saveConfig() {
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  writeEEPROMString(EEPROM_ADDR_SSID, cfgSSID, EEPROM_ADDR_SSID_LEN);
  writeEEPROMString(EEPROM_ADDR_PASS, cfgPass, EEPROM_ADDR_PASS_LEN);
  writeEEPROMString(EEPROM_ADDR_WEBUSER, cfgWebUser, EEPROM_ADDR_WEBUSER_LEN);
  writeEEPROMString(EEPROM_ADDR_WEBPASS, cfgWebPass, EEPROM_ADDR_WEBPASS_LEN);
  EEPROM.write(EEPROM_ADDR_MAC_FLAG, cfgMacCustomEnabled ? EEPROM_MAGIC : 0);
  writeEEPROMBytes(EEPROM_ADDR_MAC, cfgMacAddr, EEPROM_ADDR_MAC_LEN);
  EEPROM.commit();
}

// ---------------------------------------------------------------------
// MAC personalizada (v0.7)
// ---------------------------------------------------------------------
// Motivo: con varios ESP en la misma red domestica (satelites WiFi futuros,
// otras placas de pruebas) puede darse una colision de MAC si dos chips
// acaban con la misma MAC de fabrica clonada/reflasheada, o simplemente se
// quiere fijar una MAC concreta y estable para reservarle IP en el router
// por DHCP. Con esto se puede elegir la MAC desde /  (formulario) en vez de
// tener que tocar codigo y reflashear.

// Convierte "AA:BB:CC:DD:EE:FF" (tambien admite '-') a 6 bytes. Devuelve
// false si el formato no es exactamente ese (evita guardar basura en EEPROM
// que luego de un problema al aplicar la MAC en el arranque).
bool parseMacString(const String &s, uint8_t *out) {
  const char *p = s.c_str();
  for (int i = 0; i < 6; i++) {
    if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return false;
    char buf[3] = {p[0], p[1], '\0'};
    out[i] = (uint8_t)strtol(buf, nullptr, 16);
    p += 2;
    if (i < 5) {
      if (*p != ':' && *p != '-') return false;
      p++;
    }
  }
  return *p == '\0';
}

// Rechaza MACs que no tengan sentido como MAC de host: multicast (bit menos
// significativo del primer byte a 1) o todo-ceros. No obligamos a que sea
// "localmente administrada" (bit 0x02) porque el usuario puede querer fijar
// deliberadamente una MAC de un fabricante conocido para pruebas.
bool isMacUnicastValid(const uint8_t *mac) {
  if (mac[0] & 0x01) return false;
  bool allZero = true;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0) allZero = false;
  }
  return !allZero;
}

// Sugerencia de MAC aleatoria para el boton "Generar MAC aleatoria" del
// formulario. Mantiene SIEMPRE nuestro prefijo (OUI) 84:2B:BC -- solo los
// 3 bytes finales son aleatorios -- para que cualquier MAC que generemos
// nosotros (por defecto o sugerida) tenga el mismo formato reconocible
// 84:2B:BC:xx:xx:xx en la red. Util cuando hay varias placas y la MAC por
// defecto (derivada del chip ID) coincidiera o se quiera elegir otra a
// mano dentro del mismo prefijo. Si el usuario quiere un prefijo distinto,
// puede escribirlo el mismo directamente en el campo del formulario.
void generateRandomLocalMac(uint8_t *out) {
  out[0] = DEFAULT_MAC_OUI_0;
  out[1] = DEFAULT_MAC_OUI_1;
  out[2] = DEFAULT_MAC_OUI_2;
  for (int i = 3; i < 6; i++) out[i] = (uint8_t)random(0, 256);
}

// MAC por defecto de esta red: siempre empieza por 84:2B:BC (nuestro
// prefijo) y los 3 bytes finales salen del chip ID de cada ESP, asi que
// es estable en cada reinicio (no cambia solo porque se reinicie) pero
// distinta entre placas -- evita colisiones sin tener que configurar nada
// a mano. Se usa mientras no haya una MAC personalizada guardada.
void generateDefaultMac(uint8_t *out) {
  uint32_t chipId = ESP.getChipId();
  out[0] = DEFAULT_MAC_OUI_0;
  out[1] = DEFAULT_MAC_OUI_1;
  out[2] = DEFAULT_MAC_OUI_2;
  out[3] = (uint8_t)(chipId >> 16);
  out[4] = (uint8_t)(chipId >> 8);
  out[5] = (uint8_t)(chipId);
}

String macToString(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Aplica la MAC a las interfaces STA y AP del ESP8266. Hay que llamarla
// DESPUES de WiFi.mode(...) y ANTES de WiFi.begin()/softAP() --
// wifi_set_macaddr() necesita el modo ya puesto, y para que la
// conexion/AP arranque ya con la MAC nueva desde el primer paquete
// (algunos routers registran la MAC del primer frame DHCP/ARP).
//
// Si hay una MAC personalizada guardada, se usa esa. Si no, se usa la
// MAC por defecto de nuestra red (prefijo 84:2B:BC + bytes del chip ID),
// en vez de dejar la MAC de fabrica del chip.
void applyMac() {
  uint8_t effectiveMac[6];
  const char *origen;
  if (cfgMacCustomEnabled) {
    memcpy(effectiveMac, cfgMacAddr, 6);
    origen = "personalizada";
  } else {
    generateDefaultMac(effectiveMac);
    origen = "por defecto (84:2B:BC)";
  }
  bool okSta = wifi_set_macaddr(STATION_IF, effectiveMac);
  bool okAp = wifi_set_macaddr(SOFTAP_IF, effectiveMac);
  evLogfL(LOG_LVL_WARN, "[MAC] Aplicando MAC %s: %s (STA %s, AP %s)",
          origen, macToString(effectiveMac).c_str(), okSta ? "OK" : "FALLO", okAp ? "OK" : "FALLO");
}

// ---------------------------------------------------------------------
// Conectividad: STA con fallback a AP
// ---------------------------------------------------------------------
void startAPFallback() {
  isAPMode = true;
  apSSID = String(AP_SSID_PREFIX) + String(ESP.getChipId(), HEX);
  WiFi.mode(WIFI_AP);
  applyMac();
  WiFi.softAPConfig(AP_FIXED_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(apSSID.c_str()); // TODO: decidir si el AP lleva password o es abierto
  IPAddress ip = WiFi.softAPIP();
  evLogf("[WiFi] Modo AP SSID=%s IP=%d.%d.%d.%d MAC=%s", apSSID.c_str(), ip[0], ip[1], ip[2], ip[3], WiFi.softAPmacAddress().c_str());
}

void connectWiFi() {
  evLogf("=== esp8266_wifi arrancando (v%d.%d) ===", ESP_FW_VERSION_MAJOR, ESP_FW_VERSION_MINOR);
  if (strlen(cfgSSID) == 0) {
    evLogf("[WiFi] Sin SSID guardado en EEPROM -> AP directo");
    startAPFallback();
    return;
  }

  evLogf("[WiFi] Conectando a SSID guardado: %s", cfgSSID);
  WiFi.mode(WIFI_STA);
  applyMac();
  WiFi.begin(cfgSSID, cfgPass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
    delay(200);
    DBG("."); // progreso visual solo por Serial, no vale la pena en el log web
  }

  if (WiFi.status() == WL_CONNECTED) {
    isAPMode = false;
    IPAddress ip = WiFi.localIP();
    evLogf("[WiFi] Conectado. IP=%d.%d.%d.%d RSSI=%d MAC=%s", ip[0], ip[1], ip[2], ip[3], WiFi.RSSI(), WiFi.macAddress().c_str());
  } else {
    evLogfL(LOG_LVL_WARN, "[WiFi] Timeout conectando a %s -> fallback a AP", cfgSSID);
    startAPFallback();
  }
}

// ---------------------------------------------------------------------
// Estado del Mega (a partir del heartbeat) — ver z21_protocol.h
// ---------------------------------------------------------------------
unsigned long lastHeartbeatReceivedMs = 0; // 0 = nunca se ha recibido ninguno
uint32_t megaUptimeMs = 0;
uint32_t megaCycleAvgUs = 0;
uint16_t megaCycleMaxUs = 0;
uint16_t megaFreeRam = 0;
uint8_t megaFramesOk = 0;
uint8_t megaFramesBad = 0;
uint8_t megaFwVersionMajor = 0;
uint8_t megaFwVersionMinor = 0;
uint8_t megaStatusCode = 0;
uint8_t lastMegaFrameType = 0;
uint8_t lastMegaFrameLen = 0;
uint8_t lastMegaFramePayload[8] = {0};
uint8_t lastMegaFramePayloadLen = 0;
unsigned long lastMegaFrameAtMs = 0;

bool isMegaOnline() {
  if (lastHeartbeatReceivedMs == 0) return false; // todavía no ha llegado ninguno
  return (millis() - lastHeartbeatReceivedMs) < HEARTBEAT_TIMEOUT_MS;
}

// ---------------------------------------------------------------------
// Sniffer de bytes crudos del enlace con el Mega (activable bajo demanda)
// ---------------------------------------------------------------------
bool sniffOn = true; // activo por defecto: no hace falta pulsar "Activar"
                      // para tener visibilidad — el coste de guardar bytes
                      // en RAM es insignificante en el ESP8266. Se puede
                      // pausar desde /sniffer?off=1 si hiciera falta.
unsigned long totalRawBytesFromMega = 0; // cuenta TODO lo que llega, haya
                                           // frame válido o no — si esto
                                           // nunca sube, no hay comunicación
                                           // física en absoluto.
#define SNIFF_LOG_SIZE 512 // suficiente para varios segundos de tráfico real
uint8_t sniffLog[SNIFF_LOG_SIZE];
uint16_t sniffLogCount = 0; // bytes válidos en el log (hasta SNIFF_LOG_SIZE)
uint16_t sniffLogHead = 0;  // siguiente posición a escribir (circular)

void recordRawByte(uint8_t b) {
  totalRawBytesFromMega++;
  if (!sniffOn) return;
  sniffLog[sniffLogHead] = b;
  sniffLogHead = (sniffLogHead + 1) % SNIFF_LOG_SIZE;
  if (sniffLogCount < SNIFF_LOG_SIZE) sniffLogCount++;
}

const char *frameTypeName(uint8_t t) {
  switch (t) {
    case FRAME_TYPE_Z21: return "Z21";
    case FRAME_TYPE_HEARTBEAT: return "HEARTBEAT";
    case FRAME_TYPE_HELLO: return "HELLO";
    case FRAME_TYPE_NET_INFO: return "NET_INFO";
    case FRAME_TYPE_SYNC_ACK: return "SYNC_ACK";
    default: return "?";
  }
}

// ---------------------------------------------------------------------
// Log de TRAMAS del sniffer (v0.7) -- antes el sniffer solo capturaba los
// bytes crudos en un sentido (Mega->ESP) y los redecodificaba offline al
// pedir la pagina, mostrando como mucho "tipo/len/CHK OK/payload en hex".
// Eso era suficiente para saber si el framing interno funcionaba, pero
// nada util para depurar el protocolo Z21 en si (que header ha pedido la
// app Z21, si es un LAN_X, etc.) ni para ver que contesta el ESP.
//
// Ahora se registra cada trama ya decodificada (tipo + payload) en el
// mismo instante en que se procesa, en ambos sentidos:
//   - sniffRecordFrame(SNIFF_DIR_RX_FROM_MEGA, ...) se llama dentro de
//     tryReadFromMega(), justo tras validar el checksum de framing.
//   - sniffRecordFrame(SNIFF_DIR_TX_TO_MEGA, ...) se llama dentro de
//     sendToMega(), con el payload tal cual se envia.
// Solo se registran tramas con checksum de framing correcto (las que
// fallan ya se cuentan aparte en framesRxChkFailFromMega y se ven en el
// log de eventos) -- así el sniffer decodificado no se llena de ruido.
// El contador de bytes crudos (totalRawBytesFromMega) y su vista hexadecimal
// en bruto (/sniffer?raw=1) se mantienen tal cual estaban: siguen siendo
// la señal mas basica para saber si hay comunicacion fisica en absoluto,
// independientemente de si el framing/protocolo es correcto.
// ---------------------------------------------------------------------
#define SNIFF_DIR_RX_FROM_MEGA 0
#define SNIFF_DIR_TX_TO_MEGA 1
#define SNIFF_FRAME_LOG_SIZE 60
#define SNIFF_FRAME_PAYLOAD_PREVIEW 16

struct SniffFrameEntry {
  unsigned long ms;
  uint8_t dir;
  uint8_t type;
  uint8_t len;          // longitud real de la trama (puede ser > preview)
  uint8_t payload[SNIFF_FRAME_PAYLOAD_PREVIEW];
  uint8_t payloadLen;   // bytes realmente guardados (recortado a preview)
};

SniffFrameEntry sniffFrameLog[SNIFF_FRAME_LOG_SIZE];
uint8_t sniffFrameCount = 0;
uint8_t sniffFrameHead = 0;

void sniffRecordFrame(uint8_t dir, uint8_t type, const uint8_t *payload, uint8_t len) {
  if (!sniffOn) return;
  SniffFrameEntry &e = sniffFrameLog[sniffFrameHead];
  e.ms = millis();
  e.dir = dir;
  e.type = type;
  e.len = len;
  e.payloadLen = (len < SNIFF_FRAME_PAYLOAD_PREVIEW) ? len : SNIFF_FRAME_PAYLOAD_PREVIEW;
  for (uint8_t i = 0; i < e.payloadLen; i++) e.payload[i] = payload[i];
  sniffFrameHead = (sniffFrameHead + 1) % SNIFF_FRAME_LOG_SIZE;
  if (sniffFrameCount < SNIFF_FRAME_LOG_SIZE) sniffFrameCount++;
}

uint8_t sniffFrameChronoIndex(uint8_t i) {
  uint8_t start = (sniffFrameCount < SNIFF_FRAME_LOG_SIZE) ? 0 : sniffFrameHead;
  return (start + i) % SNIFF_FRAME_LOG_SIZE;
}

// Decodifica el header de un datagrama Z21 LAN real (el payload de una
// trama FRAME_TYPE_Z21): DataLen(2, LE) + Header(2, LE) + Data...
// Solo se nombran los headers que ya son "fuente de verdad" en
// z21_protocol.h -- el resto se muestra como "header no catalogado" en vez
// de inventarse nombres, para no dar la sensación de soportar algo que en
// realidad mega_z21.ino todavía no implementa.
String z21CommandDescribe(const uint8_t *data, uint8_t len) {
  if (len < 4) return "datagrama Z21 demasiado corto para tener header";
  uint16_t dataLen = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  uint16_t header = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
  String desc = "Header=0x" + String(header, HEX) + " (DataLen declarado=" + String(dataLen) + ")";
  switch (header) {
    case LAN_GET_SERIAL_NUMBER: desc += " LAN_GET_SERIAL_NUMBER"; break;
    case LAN_GET_HWINFO: desc += " LAN_GET_HWINFO"; break;
    case LAN_GET_CODE: desc += " LAN_GET_CODE"; break;
    case LAN_LOGOFF: desc += " LAN_LOGOFF"; break;
    case LAN_SET_BROADCASTFLAGS: desc += " LAN_SET_BROADCASTFLAGS"; break;
    case LAN_GET_BROADCASTFLAGS: desc += " LAN_GET_BROADCASTFLAGS"; break;
    case LAN_SYSTEMSTATE_GETDATA: desc += " LAN_SYSTEMSTATE_GETDATA"; break;
    case LAN_SYSTEMSTATE_DATACHANGED: desc += " LAN_SYSTEMSTATE_DATACHANGED"; break;
    case LAN_X_GET_VERSION:
      // LAN_X_GET_VERSION y LAN_X_GET_STATUS comparten el mismo header
      // 0x40 (es el canal X-Bus); el X-Header real (primer byte de Data)
      // es el que distingue el comando concreto dentro del canal.
      desc += " LAN_X (canal X-Bus)";
      if (len >= 5) desc += " XHeader=0x" + String(data[4], HEX);
      break;
    default: desc += " (header no catalogado en z21_protocol.h)"; break;
  }
  return desc;
}

// Vista principal de /sniffer: tramas ya decodificadas, mas recientes
// primero, con direccion, tipo y (si es FRAME_TYPE_Z21) el comando Z21.
// dirFilter: 0xFF = todas, o SNIFF_DIR_RX_FROM_MEGA / SNIFF_DIR_TX_TO_MEGA.
String decodeSniffFrameLog(uint8_t dirFilter) {
  if (sniffFrameCount == 0) return "(sin tramas capturadas todavia)\n";
  String out = "";
  bool any = false;
  for (int i = (int)sniffFrameCount - 1; i >= 0; i--) {
    uint8_t idx = sniffFrameChronoIndex((uint8_t)i);
    SniffFrameEntry &e = sniffFrameLog[idx];
    if (dirFilter != 0xFF && e.dir != dirFilter) continue;
    any = true;
    unsigned long ageMs = millis() - e.ms;
    out += "[+" + String(ageMs / 1000) + "." + String((ageMs % 1000) / 100) + "s] ";
    out += (e.dir == SNIFF_DIR_RX_FROM_MEGA)
             ? "<span style='color:#59f'>MEGA-&gt;ESP</span> "
             : "<span style='color:#f95'>ESP-&gt;MEGA</span> ";
    out += "<b>" + String(frameTypeName(e.type)) + "</b> len=" + String(e.len);
    if (e.type == FRAME_TYPE_Z21) {
      out += " :: " + z21CommandDescribe(e.payload, e.payloadLen);
    }
    out += " data=";
    for (uint8_t k = 0; k < e.payloadLen; k++) {
      char h[4];
      snprintf(h, sizeof(h), "%02X ", e.payload[k]);
      out += h;
    }
    if (e.len > e.payloadLen) out += "...";
    out += "\n";
  }
  if (!any) out += "(sin tramas capturadas todavia en ese sentido)\n";
  return out;
}

// ---------------------------------------------------------------------
// Enlace con el Mega (Serial) — framing con sync bytes + checksum:
// [SYNC0][SYNC1][TYPE][LEN][payload][CHK] (ver z21_protocol.h). El
// checksum permite descartar un frame corrupto y resincronizar buscando
// el siguiente 0xAA 0x55 en vez de desalinearse para siempre — mismo
// esquema que mega_z21.ino, no tocar uno sin tocar el otro.
// ---------------------------------------------------------------------
void sendToMega(uint8_t type, const uint8_t *payload, uint8_t len) {
  sniffRecordFrame(SNIFF_DIR_TX_TO_MEGA, type, payload, len);
  uint8_t chk = type ^ len;
  for (uint8_t i = 0; i < len; i++) chk ^= payload[i];
  Serial.write(LINK_SYNC_BYTE_0);
  Serial.write(LINK_SYNC_BYTE_1);
  Serial.write(type);
  Serial.write(len);
  Serial.write(payload, len);
  Serial.write(chk);
}

// Máquina de estados no bloqueante: NO consume ningún byte "a ciegas"
// hasta que el frame completo esté disponible, para no perder la
// sincronía si el payload llega repartido en varias vueltas de loop().
// Si el checksum falla, se descarta el frame y se vuelve a buscar sync
// desde cero en vez de asumir que el siguiente byte es un TYPE válido.
enum LinkState { LINK_WAIT_SYNC0, LINK_WAIT_SYNC1, LINK_WAIT_TYPE, LINK_WAIT_LEN, LINK_WAIT_PAYLOAD, LINK_WAIT_CHK };
LinkState linkState = LINK_WAIT_SYNC0;
uint8_t linkPendingType = 0;
uint8_t linkPendingLen = 0;
uint8_t linkPendingCount = 0;
uint8_t linkPendingChk = 0;
uint8_t linkPendingBuf[256];
uint32_t framesRxChkFailFromMega = 0; // frames descartados por checksum de framing

// IMPORTANTE: maxLen es uint16_t, NO uint8_t. Se llama con UDP_BUF_SIZE=256,
// y 256 no cabe en un uint8_t (se trunca a 256 % 256 = 0 en compilación) —
// con maxLen=0, outLen salía SIEMPRE 0 sin importar el frame real recibido
// del Mega. Esto rompía dos cosas a la vez: (1) handleHeartbeatFrame()
// descartaba el heartbeat por "demasiado corto" (len=0 < 17) y el Mega
// nunca se marcaba online aunque el enlace físico funcionase bien; (2) el
// reenvío FRAME_TYPE_Z21 -> UDP mandaba a la app Z21 paquetes de 0 bytes
// en vez de la respuesta real, por eso la app nunca reconocía la central
// y reintentaba LAN_X_GET_STATUS sin parar. Mismo bug, mismo fix, que en
// tryReadFrameFromESP() del Mega — no revertir sin releer ese comentario.
bool tryReadFromMega(uint8_t &outType, uint8_t *buf, uint16_t maxLen, uint8_t &outLen) {
  while (Serial.available() > 0) {
    uint8_t b = Serial.read();
    recordRawByte(b);

    switch (linkState) {
      case LINK_WAIT_SYNC0:
        if (b == LINK_SYNC_BYTE_0) linkState = LINK_WAIT_SYNC1;
        break;

      case LINK_WAIT_SYNC1:
        if (b == LINK_SYNC_BYTE_1) {
          linkState = LINK_WAIT_TYPE;
        } else if (b != LINK_SYNC_BYTE_0) {
          linkState = LINK_WAIT_SYNC0;
        }
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
        if (linkPendingCount < sizeof(linkPendingBuf)) {
          linkPendingBuf[linkPendingCount] = b;
        }
        linkPendingChk ^= b;
        linkPendingCount++;
        if (linkPendingCount >= linkPendingLen) {
          linkState = LINK_WAIT_CHK;
        }
        break;

      case LINK_WAIT_CHK:
        linkState = LINK_WAIT_SYNC0;
        if (b != linkPendingChk) {
          framesRxChkFailFromMega++;
          static unsigned long lastChkFailLogMs = 0;
          if (evLogThrottle(lastChkFailLogMs, 500)) {
            evLogfL(LOG_LVL_WARN, "[MEGA] fallo checksum framing (total=%lu)", framesRxChkFailFromMega);
          }
          break;
        }
        outType = linkPendingType;
        outLen = (linkPendingLen < maxLen) ? linkPendingLen : maxLen;
        for (uint8_t i = 0; i < outLen; i++) buf[i] = linkPendingBuf[i];
        sniffRecordFrame(SNIFF_DIR_RX_FROM_MEGA, outType, buf, outLen);
        return true;
    }
  }
  return false;
}

void handleHeartbeatFrame(const uint8_t *hb, uint8_t len) {
  if (len < HEARTBEAT_PAYLOAD_LEN) return; // frame corrupto/corto, se ignora
  bool wasOffline = (lastHeartbeatReceivedMs == 0);
  lastHeartbeatReceivedMs = millis();
  megaUptimeMs = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8) | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
  megaCycleAvgUs = (uint32_t)hb[4] | ((uint32_t)hb[5] << 8) | ((uint32_t)hb[6] << 16) | ((uint32_t)hb[7] << 24);
  megaCycleMaxUs = (uint16_t)hb[8] | ((uint16_t)hb[9] << 8);
  megaFreeRam = (uint16_t)hb[10] | ((uint16_t)hb[11] << 8);
  megaFramesOk = hb[12];
  megaFramesBad = hb[13];
  megaFwVersionMajor = hb[14];
  megaFwVersionMinor = hb[15];
  megaStatusCode = hb[16];
  if (wasOffline) {
    evLogf("[MEGA] primer heartbeat recibido: fw=v%d.%d status=%s", megaFwVersionMajor, megaFwVersionMinor, statusCodeText(megaStatusCode));
  }
}

// ---------------------------------------------------------------------
// Sincronización inicial con el Mega (antes de levantar el UDP Z21, ver
// z21_protocol.h y AGENT.md). El Mega manda HELLO hasta que le contestamos
// con NET_INFO; en cuanto llega su SYNC_ACK, megaSynced=true y a partir de
// ahí handleZ21Udp() empieza a reenviar tráfico real hacia el Mega.
// ---------------------------------------------------------------------
bool megaSynced = false;
unsigned long lastNetInfoSentMs = 0;

// Arma y manda al Mega el payload completo de NetInfo (ver formato en
// z21_protocol.h): modo, IP local, IP de la puerta de enlace (gateway),
// la MAC que tiene aplicada de verdad la interfaz activa (personalizada o
// la por defecto 84:2B:BC, ver applyMac()) y el SSID. Antes solo se
// mandaba el byte de modo -- el resto del formato estaba documentado en
// el header pero nunca se llegaba a rellenar aqui (codigo muerto); ahora
// se manda todo lo que el Mega ya sabia interpretar (ip/ssid) mas gateway
// y MAC, nuevos en v0.9.
void buildAndSendNetInfo() {
  uint8_t mode = isAPMode ? NET_INFO_MODE_AP : NET_INFO_MODE_STA;
  IPAddress ip = isAPMode ? WiFi.softAPIP() : WiFi.localIP();
  // En modo AP el propio ESP es la puerta de enlace de sus clientes; en
  // modo STA es la del router al que nos hemos conectado.
  IPAddress gw = isAPMode ? WiFi.softAPIP() : WiFi.gatewayIP();
  const char *ssid = isAPMode ? apSSID.c_str() : cfgSSID;
  uint8_t ssidLen = (uint8_t)strlen(ssid);
  if (ssidLen > NET_INFO_SSID_MAXLEN) ssidLen = NET_INFO_SSID_MAXLEN;

  uint8_t mac[NET_INFO_MAC_LEN];
  if (isAPMode) {
    WiFi.softAPmacAddress(mac);
  } else {
    WiFi.macAddress(mac);
  }

  uint8_t payload[1 + 4 + 4 + NET_INFO_MAC_LEN + 1 + NET_INFO_SSID_MAXLEN];
  uint8_t idx = 0;
  payload[idx++] = mode;
  for (int i = 0; i < 4; i++) payload[idx++] = ip[i];
  for (int i = 0; i < 4; i++) payload[idx++] = gw[i];
  memcpy(&payload[idx], mac, NET_INFO_MAC_LEN);
  idx += NET_INFO_MAC_LEN;
  payload[idx++] = ssidLen;
  memcpy(&payload[idx], ssid, ssidLen);
  idx += ssidLen;

  sendToMega(FRAME_TYPE_NET_INFO, payload, idx);
}

// Se llama en cada vuelta de loop() mientras !megaSynced: si llega HELLO
// del Mega contestamos al momento; si no, insistimos igualmente cada
// SYNC_HELLO_INTERVAL_MS por si el HELLO se perdió (framing, timing, etc.)
void handleFrameFromMegaDuringSync(uint8_t type, const uint8_t *buf, uint8_t len) {
  if (type == FRAME_TYPE_HELLO) {
    evLogf("[SYNC] HELLO recibido del Mega, respondo NET_INFO");
    buildAndSendNetInfo();
    lastNetInfoSentMs = millis();
  } else if (type == FRAME_TYPE_SYNC_ACK) {
    evLogf("[SYNC] SYNC_ACK recibido -> sincronizado, levanto el UDP Z21");
    megaSynced = true;
  } else if (type == FRAME_TYPE_HEARTBEAT) {
    handleHeartbeatFrame(buf, len);
    evLogf("[SYNC] Heartbeat recibido antes del ACK -> doy por sincronizado igualmente");
    megaSynced = true;
  } else if (type == FRAME_TYPE_NET_INFO) {
    // Fallback: si el Mega nos responde con otro tipo de frame de enlace,
    // consideramos que la línea ya está viva y empezamos a reenvía tráfico.
    evLogf("[SYNC] Frame inesperado (NET_INFO) del Mega -> doy por sincronizado igualmente");
    megaSynced = true;
  }
  // FRAME_TYPE_Z21 no debería llegar todavía (el Mega no lo manda hasta
  // synced==true en su lado); si llegara, se ignora aquí sin más.
}

const char *statusCodeText(uint8_t code) {
  switch (code) {
    case STATUS_OK: return "OK";
    case STATUS_NO_FRAMES_EVER: return "Nunca ha llegado un datagrama Z21 valido al Mega";
    case STATUS_BAD_FRAMES: return "Hay frames Z21 corruptos/mal formados";
    case STATUS_WATCHDOG_RECOVERED: return "El ultimo arranque fue un reset del watchdog (el firmware se colgo)";
    default: return "Desconocido";
  }
}

// ---------------------------------------------------------------------
// Paso transparente UDP (Z21) <-> Serial (Mega)
// ---------------------------------------------------------------------
#define UDP_BUF_SIZE 256
uint8_t udpBuf[UDP_BUF_SIZE];

void recordLastFrameFromMega(uint8_t type, const uint8_t *buf, uint8_t len) {
  lastMegaFrameType = type;
  lastMegaFrameLen = len;
  lastMegaFramePayloadLen = (len < sizeof(lastMegaFramePayload)) ? len : sizeof(lastMegaFramePayload);
  for (uint8_t i = 0; i < lastMegaFramePayloadLen; i++) {
    lastMegaFramePayload[i] = buf[i];
  }
  lastMegaFrameAtMs = millis();
}

unsigned long udpPacketsSeen = 0; // se incrementa SIEMPRE que parsePacket()>0,
                                    // incluso durante la sincronizacion — para
                                    // saber si el UDP llega al socket del ESP
                                    // en absoluto, independientemente de si se
                                    // reenvia al Mega o no.
IPAddress lastUdpSourceIP;
uint16_t lastUdpSourcePort = 0;

void handleZ21Udp() {
  // Mientras no se complete el handshake con el Mega, no levantamos el
  // reenvío UDP<->Mega: solo tramitamos frames de sincronización. Así el
  // Mega nunca se ve un datagrama Z21 antes de estar listo, y evitamos
  // desperdiciar el socket UDP con tráfico que de todas formas se
  // ignoraría en el otro extremo.
  if (!megaSynced) {
    // Aunque todavía no reenviamos tráfico al Mega, sí queremos saber si
    // llega algo al socket UDP — si esto nunca sube ni siquiera aquí, el
    // problema es de red (WiFi/IP/puerto), no del handshake con el Mega.
    int peekSize = z21Udp.parsePacket();
    if (peekSize > 0) {
      udpPacketsSeen++;
      lastUdpSourceIP = z21Udp.remoteIP();
      lastUdpSourcePort = z21Udp.remotePort();
      evLogf("[UDP] (durante sync) de %d.%d.%d.%d:%u tam=%d",
             lastUdpSourceIP[0], lastUdpSourceIP[1], lastUdpSourceIP[2], lastUdpSourceIP[3],
             lastUdpSourcePort, peekSize);
      z21Udp.flush(); // descartado: todavia no hay a quien reenviarlo
    }

    uint8_t megaType = 0;
    uint8_t megaBuf[UDP_BUF_SIZE];
    uint8_t megaLen = 0;
    if (tryReadFromMega(megaType, megaBuf, UDP_BUF_SIZE, megaLen)) {
      recordLastFrameFromMega(megaType, megaBuf, megaLen);
      handleFrameFromMegaDuringSync(megaType, megaBuf, megaLen);
    }

    // Fallback: si ya hemos enviado NET_INFO y el Mega responde con cualquier
    // frame válido, empezamos a reenviar tráfico aunque no hayamos visto el
    // SYNC_ACK explícito. Esto evita quedarse bloqueado por un ACK que no
    // llega bien por el enlace físico o la máquina de estados del ESP.
    if (!megaSynced && lastNetInfoSentMs != 0 && (lastMegaFrameAtMs != 0 || totalRawBytesFromMega > 0)) {
      megaSynced = true;
    }

    if (lastNetInfoSentMs == 0 || (millis() - lastNetInfoSentMs) >= SYNC_HELLO_INTERVAL_MS) {
      // Reintento proactivo por si el HELLO del Mega no llegó o se perdió
      // (útil justo cuando el enlace acaba de sincronizar bytes de nuevo).
      buildAndSendNetInfo();
      lastNetInfoSentMs = millis();
    }
    if (megaSynced) {
      return;
    }
    return;
  }

  int packetSize = z21Udp.parsePacket();
  if (packetSize > 0 && packetSize <= UDP_BUF_SIZE) {
    udpPacketsSeen++;
    lastUdpSourceIP = z21Udp.remoteIP();
    lastUdpSourcePort = z21Udp.remotePort();
    lastClientIP = z21Udp.remoteIP();
    lastClientPort = z21Udp.remotePort();
    int len = z21Udp.read(udpBuf, UDP_BUF_SIZE);

    char hexPreview[3 * 16 + 1] = "";
    uint8_t hexLen = (len < 16) ? (uint8_t)len : 16;
    for (uint8_t i = 0; i < hexLen; i++) {
      char b[4];
      snprintf(b, sizeof(b), "%02X ", udpBuf[i]);
      strcat(hexPreview, b);
    }
    evLogf("[UDP] de %d.%d.%d.%d:%u len=%d data=%s%s",
           lastUdpSourceIP[0], lastUdpSourceIP[1], lastUdpSourceIP[2], lastUdpSourceIP[3],
           lastUdpSourcePort, len, hexPreview, (len > 16) ? "..." : "");

    sendToMega(FRAME_TYPE_Z21, udpBuf, (uint8_t)len);

    // TODO (fase 2): si el sniffer está activo, también volcar este frame
    // por WebSocket antes/después de reenviarlo al Mega.
  }

  uint8_t megaType = 0;
  uint8_t megaBuf[UDP_BUF_SIZE];
  uint8_t megaLen = 0;
  if (tryReadFromMega(megaType, megaBuf, UDP_BUF_SIZE, megaLen)) {
    recordLastFrameFromMega(megaType, megaBuf, megaLen);
    if (megaType == FRAME_TYPE_HEARTBEAT) {
      handleHeartbeatFrame(megaBuf, megaLen);
    } else if (megaType == FRAME_TYPE_HELLO) {
      // El Mega se reinició y vuelve a pedir sincronización (p.ej. reset
      // del watchdog): contestamos igual, sin volver a bloquear el UDP.
      buildAndSendNetInfo();
    } else if (megaType == FRAME_TYPE_Z21 && lastClientPort != 0) {
      z21Udp.beginPacket(lastClientIP, lastClientPort);
      z21Udp.write(megaBuf, megaLen);
      z21Udp.endPacket();

      // TODO (fase 2): volcar también esta respuesta por WebSocket si el
      // sniffer está activo.
    }
  }
}

// ---------------------------------------------------------------------
// Servidor web de configuración (con autenticación básica HTTP)
// ---------------------------------------------------------------------
bool checkWebAuth() {
  if (!webServer.authenticate(cfgWebUser, cfgWebPass)) {
    webServer.requestAuthentication();
    return false;
  }
  return true;
}

void handleRoot() {
  if (!checkWebAuth()) return;

  String html = "<html><head><meta charset='utf-8'><title>Z21 Emulator Debug</title></head><body>";
  html += "<h1>Z21 Emulator - Debug</h1>";
  html += "<p><a href='/'>Estado</a> | <a href='/sniffer'>Sniffer</a> | <a href='/log'>Log</a> | <a href='/test'>Prueba de enlace</a> | <a href='/save'>Config</a></p>";
  html += "<h2>Estado general</h2>";
  html += "<p>Firmware ESP: v" + String(ESP_FW_VERSION_MAJOR) + "." + String(ESP_FW_VERSION_MINOR) + "</p>";
  html += "<p>Modo actual: " + String(isAPMode ? "AP (fallback)" : "STA (conectado)") + "</p>";
  html += "<p>IP: " + (isAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "</p>";
  html += "<p>RSSI: " + String(isAPMode ? 0 : WiFi.RSSI()) + " dBm</p>";
  html += "<p>MAC efectiva: ";
  html += (isAPMode ? WiFi.softAPmacAddress() : WiFi.macAddress());
  html += cfgMacCustomEnabled ? " (personalizada)" : " (por defecto, prefijo 84:2B:BC)";
  html += "</p>";

  html += "<h2>Socket UDP Z21 (puerto " + String(Z21_UDP_PORT) + ")</h2>";
  html += "<p>Paquetes UDP vistos en total: " + String(udpPacketsSeen) + "</p>";
  if (udpPacketsSeen > 0) {
    html += "<p>Ultimo origen: " + lastUdpSourceIP.toString() + ":" + String(lastUdpSourcePort) + "</p>";
  } else {
    html += "<p style='color:red'>Ningun paquete UDP ha llegado nunca a este socket. Revisa que el cliente este en la misma red y apuntando a esta IP y puerto.</p>";
  }

  html += "<h2>Estado del Mega</h2>";
  html += "<p>Sincronizacion inicial: " + String(megaSynced ? "completada" : "en curso (esperando HELLO/SYNC_ACK)") + "</p>";
  if (isMegaOnline()) {
    html += "<p style='color:green'><b>EN LINEA</b></p>";
    html += "<p>Firmware Mega: v" + String(megaFwVersionMajor) + "." + String(megaFwVersionMinor) + "</p>";
    html += "<p>Estado: " + String(statusCodeText(megaStatusCode)) + " (codigo " + String(megaStatusCode) + ")</p>";
    html += "<p>Ultimo heartbeat hace: " + String(millis() - lastHeartbeatReceivedMs) + " ms</p>";
    html += "<p>Uptime del Mega: " + String(megaUptimeMs / 1000) + " s</p>";
    html += "<p>Tiempo de ciclo medio: " + String(megaCycleAvgUs) + " us, maximo: " + String(megaCycleMaxUs) + " us</p>";
    html += "<p>RAM libre en el Mega: " + String(megaFreeRam) + " bytes</p>";
    html += "<p>Frames Z21 recibidos OK: " + String(megaFramesOk) + ", con error: " + String(megaFramesBad) + "</p>";
  } else if (lastHeartbeatReceivedMs == 0) {
    html += "<p style='color:red'><b>NUNCA SE HA RECIBIDO UN HEARTBEAT DEL MEGA</b></p>";
    html += "<p>Revisa: DIP switch en modo MCU&lt;-&gt;ESP, selector fisico RXD3/TXD3, baudios 115200 y cableado.</p>";
  } else {
    html += "<p style='color:red'><b>SIN RESPUESTA</b> (ultimo heartbeat hace " + String(millis() - lastHeartbeatReceivedMs) + " ms)</p>";
  }

  html += "<h2>Enlace Serial Mega↔ESP</h2>";
  html += "<p>Bytes crudos totales desde el arranque: " + String(totalRawBytesFromMega) + "</p>";
  html += "<p>Frames descartados por checksum: " + String(framesRxChkFailFromMega) + "</p>";
  String lastFrameHex = "";
  for (uint8_t i = 0; i < lastMegaFramePayloadLen; i++) {
    if (i > 0) lastFrameHex += " ";
    if (lastMegaFramePayload[i] < 16) lastFrameHex += "0";
    lastFrameHex += String(lastMegaFramePayload[i], HEX);
  }
  html += "<p>Último frame del Mega: tipo=0x" + String(lastMegaFrameType, HEX) + ", len=" + String(lastMegaFrameLen) + ", payload=" + lastFrameHex + "</p>";

  // TODO: campo para la direccion de cliente XpressNet (pendiente de definir)
  html += "<h2>Configuración</h2>";
  html += "<form method='POST' action='/save'>";
  html += "SSID: <input name='ssid' value='" + String(cfgSSID) + "'><br>";
  html += "Password: <input name='pass' type='password'><br>";

  // Campo MAC: vacio = usar la MAC por defecto de esta red (prefijo
  // 84:2B:BC + bytes del chip ID), no la de fabrica del chip. Si se pasa
  // ?genmac=1 (link de abajo) se rellena con una sugerencia aleatoria para
  // revisar antes de guardar -- no se aplica nada hasta pulsar "Guardar y
  // reiniciar".
  String macFieldValue = cfgMacCustomEnabled ? macToString(cfgMacAddr) : "";
  if (webServer.hasArg("genmac")) {
    uint8_t suggested[6];
    generateRandomLocalMac(suggested);
    macFieldValue = macToString(suggested);
  }
  html += "MAC personalizada (STA y AP), vacío = por defecto (84:2B:BC:" ;
  uint8_t defMac[6];
  generateDefaultMac(defMac);
  char defTail[9];
  snprintf(defTail, sizeof(defTail), "%02X:%02X:%02X", defMac[3], defMac[4], defMac[5]);
  html += String(defTail) + "): <input name='mac' value='" + macFieldValue + "' placeholder='AA:BB:CC:DD:EE:FF'><br>";
  html += "<a href='/?genmac=1'>Generar MAC aleatoria (84:2B:BC:xx:xx:xx)</a> (revisa el campo y pulsa Guardar para aplicarla; evita colisiones con otros equipos de la red. Si quieres otro prefijo, escribelo tu directamente en el campo)<br>";

  html += "<input type='submit' value='Guardar y reiniciar'>";
  html += "</form>";
  html += "</body></html>";

  webServer.send(200, "text/html", html);
}

// Color asociado a cada nivel, para que un WARN/ERROR salte a la vista en
// medio de 60 lineas de INFO sin tener que leer una a una.
const char *evLogLevelColor(uint8_t level) {
  switch (level) {
    case LOG_LVL_WARN: return "#e6a700";
    case LOG_LVL_ERROR: return "#e44";
    default: return "#ccc";
  }
}

void handleLog() {
  if (!checkWebAuth()) return;

  if (webServer.hasArg("clear")) {
    evLogClear();
  }

  // Filtro por nivel minimo a mostrar: all (por defecto) / warn / error.
  // "warn" muestra WARN+ERROR, "error" muestra solo ERROR -- así se puede
  // ir directo a lo que importa en un enlace largo con mucho trafico OK.
  uint8_t minLevel = LOG_LVL_INFO;
  String levelArg = webServer.hasArg("level") ? webServer.arg("level") : "";
  if (levelArg == "warn") minLevel = LOG_LVL_WARN;
  else if (levelArg == "error") minLevel = LOG_LVL_ERROR;

  if (webServer.hasArg("format") && webServer.arg("format") == "json") {
    // Salida programatica (util para scripts o para un futuro panel que
    // no sea HTML) -- mismo filtro de nivel que la vista HTML.
    String json = "{\"total\":" + String(evLogTotal) +
                   ",\"warnTotal\":" + String(evLogWarnTotal) +
                   ",\"errorTotal\":" + String(evLogErrorTotal) + ",\"entries\":[";
    bool first = true;
    for (int i = (int)evLogCount - 1; i >= 0; i--) {
      uint8_t idx = evLogChronoIndex((uint8_t)i);
      if (evLog[idx].level < minLevel) continue;
      if (!first) json += ",";
      first = false;
      // Escape minimo de comillas/backslash -- los textos los generamos
      // nosotros mismos con evLogf(), no vienen de entrada de usuario.
      String text = evLog[idx].text;
      text.replace("\\", "\\\\");
      text.replace("\"", "\\\"");
      json += "{\"ageMs\":" + String(millis() - evLog[idx].ms) +
              ",\"level\":\"" + String(evLogLevelName(evLog[idx].level)) + "\"" +
              ",\"text\":\"" + text + "\"}";
    }
    json += "]}";
    webServer.send(200, "application/json", json);
    return;
  }

  String html = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'>";
  html += "<title>Log ESP</title></head><body>";
  html += "<h1>Log de eventos del ESP</h1>";
  html += "<p><a href='/'>Estado</a> | <a href='/sniffer'>Sniffer</a> | <a href='/log'>Log</a> | <a href='/test'>Prueba de enlace</a></p>";
  html += "<p>Eventos totales desde el arranque: " + String(evLogTotal) +
          " (avisos: " + String(evLogWarnTotal) + ", errores: " + String(evLogErrorTotal) + ")";
  if (evLogTotal > EVLOG_LINES) {
    html += " -- se conservan los ultimos " + String(EVLOG_LINES);
  }
  html += "</p>";
  html += "<p>Nivel minimo: ";
  html += (minLevel == LOG_LVL_INFO) ? "<b>todos</b>" : "<a href='/log'>todos</a>";
  html += " | ";
  html += (minLevel == LOG_LVL_WARN) ? "<b>avisos+errores</b>" : "<a href='/log?level=warn'>avisos+errores</a>";
  html += " | ";
  html += (minLevel == LOG_LVL_ERROR) ? "<b>solo errores</b>" : "<a href='/log?level=error'>solo errores</a>";
  html += " | <a href='/log?clear=1'>Limpiar vista</a>";
  html += " | <a href='/log?format=json'>Ver como JSON</a></p>";

  html += "<p style='font-family:monospace; white-space:pre-wrap'>";
  bool any = false;
  for (int i = (int)evLogCount - 1; i >= 0; i--) {
    uint8_t idx = evLogChronoIndex((uint8_t)i);
    if (evLog[idx].level < minLevel) continue;
    any = true;
    unsigned long ageMs = millis() - evLog[idx].ms;
    html += "<span style='color:" + String(evLogLevelColor(evLog[idx].level)) + "'>";
    html += "[+" + String(ageMs / 1000) + "." + String((ageMs % 1000) / 100) + "s] ";
    html += "[" + String(evLogLevelName(evLog[idx].level)) + "] ";
    html += evLog[idx].text;
    html += "</span>\n";
  }
  if (!any) {
    html += (evLogCount == 0) ? "(todavia no hay eventos registrados)" : "(ningun evento cumple el filtro de nivel actual)";
  }
  html += "</p></body></html>";

  webServer.send(200, "text/html", html);
}

void handleSniffer() {
  if (!checkWebAuth()) return;

  if (webServer.hasArg("on")) {
    sniffOn = true;
    sniffLogCount = 0;
    sniffLogHead = 0;
    sniffFrameCount = 0;
    sniffFrameHead = 0;
  }
  if (webServer.hasArg("off")) {
    sniffOn = false;
  }
  bool showRaw = webServer.hasArg("raw");

  // Filtro de direccion para la vista decodificada: por defecto "todas".
  uint8_t dirFilter = 0xFF;
  String dirArg = webServer.hasArg("dir") ? webServer.arg("dir") : "";
  if (dirArg == "mega") dirFilter = SNIFF_DIR_RX_FROM_MEGA;      // solo Mega->ESP
  else if (dirArg == "esp") dirFilter = SNIFF_DIR_TX_TO_MEGA;    // solo ESP->Mega

  String html = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'>";
  html += "<title>Sniffer</title></head><body>";
  html += "<h1>Sniffer Serial ESP&lt;-&gt;Mega</h1>";
  html += "<p><a href='/'>Estado</a> | <a href='/sniffer'>Sniffer</a> | <a href='/log'>Log</a> | <a href='/test'>Prueba de enlace</a></p>";
  html += "<p>Estado: " + String(sniffOn ? "ACTIVO (capturando)" : "PAUSADO") + "</p>";
  html += "<p>Bytes crudos totales desde el arranque: " + String(totalRawBytesFromMega) + "</p>";
  html += "<p>Frames descartados por checksum de framing: " + String(framesRxChkFailFromMega) + "</p>";
  html += "<p><a href='/sniffer?on=1'>Reiniciar captura</a> | <a href='/sniffer?off=1'>Pausar</a> | ";
  html += (showRaw ? "<a href='/sniffer'>Ver decodificado</a>" : "<a href='/sniffer?raw=1'>Ver hex plano</a>");
  html += " | <a href='/'>Volver</a></p>";

  if (showRaw) {
    html += "<h3>Bytes crudos capturados (hex, orden cronologico, un solo sentido Mega-&gt;ESP)</h3>";
    html += "<p>Util solo para saber si hay comunicacion fisica en absoluto; para ver el protocolo, usa la vista decodificada.</p>";
    if (sniffLogCount == 0) {
      html += "<p>Todavia no ha llegado ningun byte.</p>";
    } else {
      html += "<p style='font-family:monospace; word-break:break-all'>";
      uint16_t start = (sniffLogCount < SNIFF_LOG_SIZE) ? 0 : sniffLogHead;
      for (uint16_t i = 0; i < sniffLogCount; i++) {
        uint8_t b = sniffLog[(start + i) % SNIFF_LOG_SIZE];
        if (b < 0x10) html += "0";
        html += String(b, HEX) + " ";
      }
      html += "</p>";
    }
  } else {
    html += "<p>Direccion: ";
    html += (dirFilter == 0xFF) ? "<b>todas</b>" : "<a href='/sniffer'>todas</a>";
    html += " | ";
    html += (dirFilter == SNIFF_DIR_RX_FROM_MEGA) ? "<b>Mega-&gt;ESP</b>" : "<a href='/sniffer?dir=mega'>Mega-&gt;ESP</a>";
    html += " | ";
    html += (dirFilter == SNIFF_DIR_TX_TO_MEGA) ? "<b>ESP-&gt;Mega</b>" : "<a href='/sniffer?dir=esp'>ESP-&gt;Mega</a>";
    html += "</p>";
    html += "<h3>Tramas decodificadas (mas reciente primero, hasta " + String(SNIFF_FRAME_LOG_SIZE) + ")</h3>";
    html += "<p style='font-family:monospace; white-space:pre-wrap'>";
    html += decodeSniffFrameLog(dirFilter);
    html += "</p>";
  }
  html += "</body></html>";

  webServer.send(200, "text/html", html);
}

void handleTest() {
  if (!checkWebAuth()) return;

  if (webServer.hasArg("send")) {
    // Frame de prueba minimo a proposito (type NET_INFO pero solo con el
    // byte de modo, sin ip/gateway/mac/ssid) -- el Mega lo acepta igual
    // (handleNetInfo() deja a 0 lo que no llegue), solo sirve para
    // comprobar que el enlace serie responde, no para probar el NetInfo
    // real (eso lo manda buildAndSendNetInfo() en el handshake).
    uint8_t payload[1] = {0x01};
    sendToMega(FRAME_TYPE_NET_INFO, payload, 1);
  }

  String html = "<html><head><meta charset='utf-8'><title>Prueba de enlace</title></head><body>";
  html += "<h1>Prueba de enlace Mega↔ESP</h1>";
  html += "<p><a href='/'>Estado</a> | <a href='/sniffer'>Sniffer</a> | <a href='/log'>Log</a> | <a href='/test'>Prueba de enlace</a></p>";
  html += "<p>Envía un frame de prueba al Mega para comprobar que el canal Serie funciona.</p>";
  html += "<p><a href='/test?send=1'>Enviar frame de prueba</a></p>";
  html += "<p>Último frame visto del Mega: tipo=0x" + String(lastMegaFrameType, HEX) + ", len=" + String(lastMegaFrameLen) + "</p>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleSave() {
  if (!checkWebAuth()) return;

  if (webServer.hasArg("ssid")) {
    webServer.arg("ssid").toCharArray(cfgSSID, EEPROM_ADDR_SSID_LEN + 1);
  }
  if (webServer.hasArg("pass") && webServer.arg("pass").length() > 0) {
    webServer.arg("pass").toCharArray(cfgPass, EEPROM_ADDR_PASS_LEN + 1);
  }

  if (webServer.hasArg("mac")) {
    String macStr = webServer.arg("mac");
    macStr.trim();
    if (macStr.length() == 0) {
      if (cfgMacCustomEnabled) evLogfL(LOG_LVL_WARN, "[CFG] MAC personalizada desactivada, se usara la de fabrica");
      cfgMacCustomEnabled = false;
    } else {
      uint8_t parsed[6];
      if (parseMacString(macStr, parsed) && isMacUnicastValid(parsed)) {
        memcpy(cfgMacAddr, parsed, sizeof(cfgMacAddr));
        cfgMacCustomEnabled = true;
        evLogfL(LOG_LVL_WARN, "[CFG] MAC personalizada guardada: %s", macToString(cfgMacAddr).c_str());
      } else {
        evLogfL(LOG_LVL_ERROR, "[CFG] MAC '%s' invalida (formato AA:BB:CC:DD:EE:FF, unicast) -- se ignora, se mantiene la anterior", macStr.c_str());
      }
    }
  }

  saveConfig();
  evLogf("[CFG] Configuracion guardada (SSID=%s), reiniciando...", cfgSSID);

  webServer.send(200, "text/html", "<html><body>Guardado. Reiniciando...</body></html>");
  delay(500);
  ESP.restart();
}

void setupWebServer() {
  webServer.on("/", handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/sniffer", handleSniffer);
  webServer.on("/log", handleLog);
  webServer.on("/test", handleTest);
  // TODO: endpoint websocket para el volcado de tramas (fase 2, mas
  // adelante — de momento la pagina /sniffer con auto-refresh es
  // suficiente para depurar el enlace Mega<->ESP)
  webServer.begin();
}

// ---------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------
void setup() {
  randomSeed(ESP.getChipId() ^ micros()); // usado solo para sugerir MACs aleatorias

  Serial.begin(115200); // enlace con el Mega, ver nota de cabecera. En
                         // producción (ESP_DEBUG_STANDALONE=0) este es su
                         // único uso — nada de texto de depuración se
                         // escribe aquí, ver evLogf()/DBG más arriba.

#if ESP_DEBUG_STANDALONE
  // Modo standalone: el Mega está desconectado (DIP en USB<->ESP), así
  // que forzamos megaSynced=true directamente para poder probar WiFi+UDP
  // sin depender del handshake HELLO/NET_INFO/SYNC_ACK (que nunca
  // llegaría, porque no hay Mega en el otro extremo).
  megaSynced = true;
  evLogf("=== MODO DEBUG STANDALONE - Mega desconectado, sync forzado ===");
#endif
  // En producción no se fuerza nada: megaSynced empieza en false y
  // handleZ21Udp() completa el handshake real en cuanto el Mega mande su
  // primer HELLO (ver z21_protocol.h y AGENT.md, "Sincronización inicial").

  loadConfig();
  connectWiFi();

  z21Udp.begin(Z21_UDP_PORT);
  evLogf("[UDP] Servidor Z21 escuchando en puerto %d", Z21_UDP_PORT);
  setupWebServer();
  evLogf("[WEB] Servidor web listo en puerto 80");
}

void loop() {
  handleZ21Udp();
  webServer.handleClient();
  // TODO: si estamos en modo AP fallback, reintentar STA periódicamente
  // en segundo plano (a definir estrategia exacta, ver spec seccion 12).
}
