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
#include <ESP8266mDNS.h> // ArduinoOTA lo usa para anunciarse como "puerto de red" en el IDE
#include <ArduinoOTA.h>
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
#include "web_assets.h"

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
// v0.12: cada SECCION de la config tiene su PROPIO byte de validez, en
// vez de un unico magic global para todo el bloque de EEPROM.
//
// Motivo (el bug que nos acaba de pasar): en v0.11 habia un solo magic
// global. Al cambiar el layout de las redes WiFi (de 1 a 3), tuvimos que
// subirlo para no leer basura -- y de paso se invalido TAMBIEN el usuario/
// password del portal web, que no habia cambiado de formato para nada.
// Resultado: credenciales "perdidas" sin haber tocado esa parte.
//
// Con un byte de validez por seccion, cambiar el layout de UNA seccion
// (p.ej. añadir una 4a red WiFi el dia de mañana) solo invalida esa
// seccion -- las demas (usuario/password del portal, MAC personalizada)
// se quedan intactas. Cada seccion es responsable solo de sus propios
// datos y de su propia validez (responsabilidad unica).
#define EEPROM_SIZE 2048
#define EEPROM_SECTION_VALID 0xC3 // "esta seccion tiene datos guardados con el layout actual"

// --- Seccion: redes WiFi guardadas (hasta 3), ver "Conectividad" abajo ---
#define EEPROM_ADDR_NET_VALID 0
#define WIFI_MAX_NETWORKS 3
#define EEPROM_ADDR_SSID_LEN 32
#define EEPROM_ADDR_PASS_LEN 64
#define EEPROM_NET_BLOCK_LEN (EEPROM_ADDR_SSID_LEN + EEPROM_ADDR_PASS_LEN) // 96 bytes/red
#define EEPROM_ADDR_NETWORKS (EEPROM_ADDR_NET_VALID + 1) // 3 bloques consecutivos desde aqui

// --- Seccion: credenciales del portal web (usuario/password de acceso) ---
#define EEPROM_ADDR_WEBAUTH_VALID (EEPROM_ADDR_NETWORKS + WIFI_MAX_NETWORKS * EEPROM_NET_BLOCK_LEN)
#define EEPROM_ADDR_WEBUSER (EEPROM_ADDR_WEBAUTH_VALID + 1)
#define EEPROM_ADDR_WEBUSER_LEN 32
#define EEPROM_ADDR_WEBPASS (EEPROM_ADDR_WEBUSER + EEPROM_ADDR_WEBUSER_LEN)
#define EEPROM_ADDR_WEBPASS_LEN 32

// --- Seccion: MAC personalizada (v0.7) ---
// Se aplica a STA y AP por igual con wifi_set_macaddr() antes de conectar
// (ver applyMac()). Si no esta guardada, NO se usa la MAC de fabrica del
// chip: se genera una MAC por defecto con el prefijo (OUI) 84:2B:BC,
// propio de nuestra red, y los 3 bytes finales derivados del chip ID
// (estable entre reinicios, distinto por placa). El usuario puede seguir
// fijando cualquier otra MAC (incluso con otro prefijo) desde el
// formulario / -- este es solo el valor por defecto.
#define EEPROM_ADDR_MAC_VALID (EEPROM_ADDR_WEBPASS + EEPROM_ADDR_WEBPASS_LEN)
#define EEPROM_ADDR_MAC (EEPROM_ADDR_MAC_VALID + 1)
#define EEPROM_ADDR_MAC_LEN 6
#define DEFAULT_MAC_OUI_0 0x84
#define DEFAULT_MAC_OUI_1 0x2B
#define DEFAULT_MAC_OUI_2 0xBC

// --- Seccion: password de OTA (actualizacion de firmware por WiFi) ---
// Deliberadamente SEPARADA de cfgWebPass (la del portal): son privilegios
// distintos -- ver/cambiar la configuracion no es lo mismo que poder
// flashear codigo arbitrario en el ESP. Si la password del portal se
// filtra, con una password de OTA distinta el atacante no gana tambien la
// capacidad de reemplazar el firmware entero.
#define EEPROM_ADDR_OTAPASS_VALID (EEPROM_ADDR_MAC + EEPROM_ADDR_MAC_LEN)
#define EEPROM_ADDR_OTAPASS (EEPROM_ADDR_OTAPASS_VALID + 1)
#define EEPROM_ADDR_OTAPASS_LEN 32

// --- Seccion: nombres "amigables" de clientes Z21 (panel /clients) ---
// Cada registro: 1 byte (keyIsMac) + 6 bytes (mac o ip, ver Z21Client mas
// abajo) + nombre. MAX_TRACKED_CLIENTS y CLIENT_NAME_LEN se definen aqui
// (no junto al resto de cosas de clientes, mas abajo) porque hacen falta
// para calcular el tamaño de esta seccion de EEPROM.
//
// 16 es un numero arbitrario, no un limite de hardware/RAM: cada entrada
// son ~44 bytes en RAM (trivial) y 32 en EEPROM, asi que subirlo mas
// (32, 50...) seguiria siendo barato -- se puede subir sin miedo si hacen
// falta mas. El cuello de botella real para muchos clientes activos a la
// vez NO esta aqui, esta en el enlace serie unico hacia el Mega (un solo
// canal, un solo microcontrolador procesando comandos uno detras de
// otro) -- eso no lo arregla agrandar esta tabla, ver comentario junto a
// CLIENT_REPLY_QUEUE_LEN mas abajo.
#define MAX_TRACKED_CLIENTS 16
#define CLIENT_NAME_LEN 24
#define EEPROM_CLIENT_RECORD_LEN (1 + 6 + (CLIENT_NAME_LEN + 1)) // keyIsMac + key + nombre
#define EEPROM_ADDR_CLIENTS_VALID (EEPROM_ADDR_OTAPASS + EEPROM_ADDR_OTAPASS_LEN)
#define EEPROM_ADDR_CLIENTS (EEPROM_ADDR_CLIENTS_VALID + 1) // MAX_TRACKED_CLIENTS registros consecutivos desde aqui

char cfgSSID[WIFI_MAX_NETWORKS][EEPROM_ADDR_SSID_LEN + 1] = {"", "", ""};
char cfgPass[WIFI_MAX_NETWORKS][EEPROM_ADDR_PASS_LEN + 1] = {"", "", ""};
// TODO: valores por defecto conocidos ("admin"/"z21admin") -- inseguro
// mientras no se fuerce un cambio en el primer arranque. handleSave() ya
// avisa por el log si se guarda dejando la password por defecto.
char cfgWebUser[EEPROM_ADDR_WEBUSER_LEN + 1] = "admin";
char cfgWebPass[EEPROM_ADDR_WEBPASS_LEN + 1] = "z21admin";
bool cfgMacCustomEnabled = false;
uint8_t cfgMacAddr[EEPROM_ADDR_MAC_LEN] = {0, 0, 0, 0, 0, 0};
char cfgOtaPass[EEPROM_ADDR_OTAPASS_LEN + 1] = "z21firmware"; // TODO: mismo aviso que cfgWebPass -- cambiar en cuanto se use en serio

// ---------------------------------------------------------------------
// Estado de red
// ---------------------------------------------------------------------
#define STA_CONNECT_TIMEOUT_MS 10000
#define AP_SSID_PREFIX "Z21_"
#define AP_FIXED_IP IPAddress(192, 168, 0, 111)
#define AP_GATEWAY IPAddress(192, 168, 0, 111)
#define AP_SUBNET IPAddress(255, 255, 255, 0)
// TODO: fijo por ahora a proposito (no se toca el Mega, que esta con otros
// procesos). Mas adelante: generar una password aleatoria por placa y
// mostrarla en la pantalla del Mega, en vez de tenerla fija en el firmware.
//
// "z21" (3 caracteres) que se uso al principio NO llegaba al minimo de 8
// que exige WPA2 -- WiFi.softAP() lo rechazaba en silencio y el AP salia
// ABIERTO en la practica, sin ningun cifrado. Se alargo a "z21admin" (8
// caracteres) reutilizando el mismo valor por defecto que ya existia para
// la password del portal web (cfgWebPass) en vez de inventar un secreto
// nuevo -- sigue siendo temporal/fijo tal como se pidio, pero ahora si
// cifra la red de verdad.
#define AP_FIXED_PASSWORD "z21admin"
// Cada cuanto, estando en modo AP, se reintenta conectar a alguna de las
// redes STA guardadas (por si vuelve a estar disponible). No se hace mas
// a menudo para no cortar a los clientes ya conectados al AP demasiado
// seguido -- cada intento implica tirar el AP y volver a levantarlo si
// falla.
#define WIFI_STA_RETRY_INTERVAL_MS 60000UL

bool isAPMode = false;
String apSSID = ""; // se rellena en startAPFallback(); usado también en NET_INFO
unsigned long apModeSinceMs = 0; // 0 = no estamos en AP; si no, marca de tiempo de cuando se entro en AP (para el reintento periodico, ver maybeRetrySTA())

WiFiUDP z21Udp;
ESP8266WebServer webServer(80);

// Buffer temporal para el archivo subido en /restore (ver handleRestoreUpload()
// / handleRestoreDone() mas abajo) -- un backup nuestro son unas pocas lineas
// de JSON, asi que un limite de RESTORE_MAX_UPLOAD_BYTES generoso evita que un
// archivo subido por error (o con mala intencion) llene la RAM del ESP.
#define RESTORE_MAX_UPLOAD_BYTES 4096
String restoreUploadBuffer;
bool restoreUploadTooBig = false;

// ---------------------------------------------------------------------
// Soporte multi-cliente Z21
// ---------------------------------------------------------------------
// ANTES: una sola variable global lastClientIP/lastClientPort, que se
// sobreescribia con cada UDP entrante. Con un solo cliente Z21 conectado
// (el caso normal hasta ahora) funciona perfecto -- pero con dos o mas
// apps conectadas a la vez, si llegan peticiones de A y de B casi
// seguidas, la respuesta que venga del Mega para A puede acabar
// mandandose a B (quien sea que haya escrito ultimo en la variable),
// simplemente por el orden de llegada, no por a quien correspondia.
//
// ARREGLO (best-effort, sin tocar el Mega): en vez de "el ultimo", se
// lleva una COLA FIFO de clientes pendientes de respuesta. Cada peticion
// UDP reenviada al Mega empuja (IP, puerto) al final de la cola; cada
// frame Z21 que vuelve del Mega hace pop del PRIMERO de la cola (asume
// que el Mega procesa y responde en el mismo orden en que le llegan las
// peticiones, que es like que su firmware sea de un solo hilo leyendo el
// serie secuencialmente -- una mejora real sobre "el ultimo que hablo",
// pero sigue siendo una heuristica).
//
// LIMITE REAL DE ESTE ENFOQUE: el framing ESP<->Mega no lleva ningun
// identificador de cliente. Si el Mega alguna vez manda una respuesta sin
// que le corresponda a la peticion que esta en la cabeza de la cola (por
// ejemplo, un frame que en realidad es un broadcast a raiz de un cambio
// de estado, no una respuesta directa a la ultima peticion), este frame
// se enviaria igualmente al cliente equivocado. Una solucion robusta de
// verdad necesitaria que el Mega etiquetase sus respuestas con un indice
// de cliente en el framing -- eso SI implica tocar el Mega, y no se ha
// hecho aqui a proposito (ver conversacion: "no tocar el Mega").
//
// Ademas: los mensajes de tipo BROADCAST del protocolo Z21 real (cambios
// de estado de una loco, de la alimentacion de via, etc.) deberian
// mandarse a TODOS los clientes conectados con el flag correspondiente
// activado (ver LAN_SET_BROADCASTFLAGS en z21_protocol.h), no solo al que
// esta en la cabeza de la cola -- eso tampoco esta implementado: hoy CADA
// frame Z21 que vuelve del Mega se trata como respuesta directa a un solo
// cliente. Con un solo cliente conectado (uso tipico hasta ahora) no se
// nota; con varios apps a la vez, cada una podria dejar de ver los
// cambios que provoquen las demas. Documentado como limitacion conocida.
//
// Este numero es sobre PETICIONES EN VUELO (mandadas al Mega, respuesta
// aun no llegada), no sobre cuantos clientes puede haber en total -- eso
// es MAX_TRACKED_CLIENTS, mas arriba, un contador distinto. En la
// practica esta cola se vacia casi al instante (el Mega responde en el
// mismo loop() o el siguiente), asi que ni con muchos clientes activos
// deberia llenarse mientras el enlace serie vaya siguiendole el ritmo.
// Subirlo es barato (unos pocos bytes por hueco) pero NO evita que el
// enlace serie sea el cuello de botella real: sigue siendo un solo canal
// y un solo microcontrolador (el Mega) procesando comandos uno detras de
// otro, igual que en la central Z21 real -- mas clientes a la vez
// escribiendo trenes significa mas comandos por segundo por ese mismo
// canal, no una cola mas larga aqui.
#define CLIENT_REPLY_QUEUE_LEN 16
struct PendingReply {
  IPAddress ip;
  uint16_t port;
};
PendingReply replyQueue[CLIENT_REPLY_QUEUE_LEN];
uint8_t replyQueueHead = 0;  // siguiente a sacar (pop)
uint8_t replyQueueCount = 0;

void replyQueuePush(IPAddress ip, uint16_t port) {
  if (replyQueueCount >= CLIENT_REPLY_QUEUE_LEN) {
    // Cola llena (demasiadas peticiones sin respuesta aun) -- se descarta
    // la mas antigua para no bloquear indefinidamente; mejor perder una
    // respuesta ocasional que dejar de aceptar peticiones nuevas.
    replyQueueHead = (replyQueueHead + 1) % CLIENT_REPLY_QUEUE_LEN;
    replyQueueCount--;
    evLogfL(LOG_LVL_WARN, "[Z21] Cola de respuestas llena, se descarta la mas antigua");
  }
  uint8_t tail = (replyQueueHead + replyQueueCount) % CLIENT_REPLY_QUEUE_LEN;
  replyQueue[tail].ip = ip;
  replyQueue[tail].port = port;
  replyQueueCount++;
}

// Devuelve true y rellena ip/port si habia algo en la cola; false si esta vacia.
bool replyQueuePop(IPAddress &ip, uint16_t &port) {
  if (replyQueueCount == 0) return false;
  ip = replyQueue[replyQueueHead].ip;
  port = replyQueue[replyQueueHead].port;
  replyQueueHead = (replyQueueHead + 1) % CLIENT_REPLY_QUEUE_LEN;
  replyQueueCount--;
  return true;
}

// ---------------------------------------------------------------------
// Registro de clientes (panel /clients): quien se ha conectado, con que
// IP/MAC, y el nombre "amigable" que se le haya puesto.
// ---------------------------------------------------------------------
// LIMITACION DE LA MAC: solo se puede averiguar la MAC de un cliente
// cuando el ESP esta en modo AP (nuestro propio AP conoce la MAC de cada
// estacion conectada, via wifi_softap_get_station_info() del SDK). Si el
// ESP esta en modo STA (conectado al router de casa, el caso normal), NO
// hay forma sencilla de obtener la MAC de otro dispositivo en esa red
// desde el ESP -- el Arduino core no expone la tabla ARP, y montar una
// resolucion ARP manual seria fragil y no aportaria fiabilidad real. En
// ese caso se seguiria por IP como identidad (keyIsMac=false); si la IP
// de ese cliente cambia (DHCP), el ESP lo vera como un cliente nuevo, sin
// poder enlazarlo con el nombre puesto antes. Es una limitacion de
// plataforma, no una decision de diseño.
#define CLIENT_TIMEOUT_MS 30000UL // sin paquetes de este cliente en 30s -> se marca desconectado

struct Z21Client {
  bool inUse;
  bool keyIsMac;       // true: identidad = mac[6]; false: identidad = ip (ver limitacion arriba)
  uint8_t mac[6];
  IPAddress ip;         // IP actual (o la unica conocida, si keyIsMac=false)
  uint16_t lastPort;
  unsigned long lastSeenMs; // 0 = no visto en este arranque (solo puede pasar en entradas cargadas de EEPROM)
  bool hasName;
  char name[CLIENT_NAME_LEN + 1];
};
Z21Client z21Clients[MAX_TRACKED_CLIENTS];

// Intenta resolver la MAC de un cliente a partir de su IP. Solo funciona
// si el ESP esta en modo AP (nuestro propio AP conoce la MAC de cada
// estacion conectada a el, via el SDK) -- ver el comentario grande mas
// arriba sobre por que en modo STA esto no es posible con las APIs
// estandar del core.
bool resolveStationMac(IPAddress ip, uint8_t *macOut) {
  if (!isAPMode) return false;

  struct station_info *station = wifi_softap_get_station_info();
  bool found = false;
  while (station != NULL) {
    IPAddress stationIp(station->ip.addr);
    if (stationIp == ip) {
      memcpy(macOut, station->bssid, 6);
      found = true;
      break;
    }
    station = STAILQ_NEXT(station, next);
  }
  wifi_softap_free_station_info();
  return found;
}

// Se llama con cada datagrama UDP Z21 entrante (ver handleZ21Udp()).
// Busca si ya conocemos a este cliente (por MAC si se puede resolver,
// si no por IP) y actualiza su "visto por ultima vez"; si es nuevo, le
// asigna un hueco libre, o desaloja al anonimo visto hace mas tiempo si
// la tabla esta llena -- los clientes CON NOMBRE puesto nunca se
// desalojan, para que el panel /clients los pueda seguir mostrando como
// "Desconectado" en vez de perderlos.
void trackClient(IPAddress ip, uint16_t port) {
  uint8_t mac[6];
  bool macKnown = resolveStationMac(ip, mac);

  int slot = -1;
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    if (!z21Clients[i].inUse) continue;
    bool sameByMac = macKnown && z21Clients[i].keyIsMac && memcmp(z21Clients[i].mac, mac, 6) == 0;
    bool sameByIpUpgrade = macKnown && !z21Clients[i].keyIsMac && z21Clients[i].ip == ip;
    bool sameByIp = !macKnown && !z21Clients[i].keyIsMac && z21Clients[i].ip == ip;
    if (sameByMac || sameByIpUpgrade || sameByIp) {
      slot = i;
      break;
    }
  }

  if (slot < 0) {
    for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
      if (!z21Clients[i].inUse) { slot = i; break; }
    }
  }
  if (slot < 0) {
    unsigned long oldest = 0xFFFFFFFFUL;
    for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
      if (z21Clients[i].hasName) continue; // nunca se desaloja a uno con nombre
      if (z21Clients[i].lastSeenMs < oldest) {
        oldest = z21Clients[i].lastSeenMs;
        slot = i;
      }
    }
  }
  if (slot < 0) return; // tabla llena de clientes con nombre, no hay hueco -- se ignora este cliente nuevo

  z21Clients[slot].inUse = true;
  z21Clients[slot].ip = ip;
  z21Clients[slot].lastPort = port;
  z21Clients[slot].lastSeenMs = millis();
  if (macKnown) {
    z21Clients[slot].keyIsMac = true;
    memcpy(z21Clients[slot].mac, mac, 6);
  }
}

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

  if (EEPROM.read(EEPROM_ADDR_NET_VALID) == EEPROM_SECTION_VALID) {
    for (uint8_t i = 0; i < WIFI_MAX_NETWORKS; i++) {
      int base = EEPROM_ADDR_NETWORKS + i * EEPROM_NET_BLOCK_LEN;
      readEEPROMString(base, cfgSSID[i], EEPROM_ADDR_SSID_LEN);
      readEEPROMString(base + EEPROM_ADDR_SSID_LEN, cfgPass[i], EEPROM_ADDR_PASS_LEN);
    }
  }
  // si no, se quedan las 3 redes vacias por defecto (-> AP directo)

  if (EEPROM.read(EEPROM_ADDR_WEBAUTH_VALID) == EEPROM_SECTION_VALID) {
    readEEPROMString(EEPROM_ADDR_WEBUSER, cfgWebUser, EEPROM_ADDR_WEBUSER_LEN);
    readEEPROMString(EEPROM_ADDR_WEBPASS, cfgWebPass, EEPROM_ADDR_WEBPASS_LEN);
  }
  // si no, se quedan admin/z21admin por defecto

  cfgMacCustomEnabled = (EEPROM.read(EEPROM_ADDR_MAC_VALID) == EEPROM_SECTION_VALID);
  if (cfgMacCustomEnabled) {
    readEEPROMBytes(EEPROM_ADDR_MAC, cfgMacAddr, EEPROM_ADDR_MAC_LEN);
  }

  if (EEPROM.read(EEPROM_ADDR_OTAPASS_VALID) == EEPROM_SECTION_VALID) {
    readEEPROMString(EEPROM_ADDR_OTAPASS, cfgOtaPass, EEPROM_ADDR_OTAPASS_LEN);
  }
  // si no, se queda "z21firmware" por defecto

  loadClientNames();
}

// Carga los nombres de clientes guardados directamente en z21Clients[],
// uno por indice -- separado de loadConfig()/saveConfig() porque poner o
// cambiar un nombre NO debe reiniciar el ESP (a diferencia de guardar la
// config de red/portal/OTA, que si lo hace). lastSeenMs se deja a 0
// (nunca visto en este arranque) hasta que el cliente vuelva a mandar
// algo -- por eso un cliente con nombre aparece "Desconectado" hasta que
// se le vea de nuevo, en vez de asumir que sigue conectado tras un reinicio.
void loadClientNames() {
  if (EEPROM.read(EEPROM_ADDR_CLIENTS_VALID) != EEPROM_SECTION_VALID) return; // nada guardado

  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    int base = EEPROM_ADDR_CLIENTS + i * EEPROM_CLIENT_RECORD_LEN;
    uint8_t keyIsMacByte = EEPROM.read(base);
    uint8_t key[6];
    readEEPROMBytes(base + 1, key, 6);
    char name[CLIENT_NAME_LEN + 1];
    readEEPROMString(base + 1 + 6, name, CLIENT_NAME_LEN);
    if (strlen(name) == 0) continue; // registro vacio -> ese slot se queda libre en RAM

    z21Clients[i].inUse = true;
    z21Clients[i].hasName = true;
    z21Clients[i].keyIsMac = (keyIsMacByte == 1);
    if (z21Clients[i].keyIsMac) {
      memcpy(z21Clients[i].mac, key, 6);
    } else {
      z21Clients[i].ip = IPAddress(key[0], key[1], key[2], key[3]);
    }
    z21Clients[i].lastPort = 0;
    z21Clients[i].lastSeenMs = 0;
    strncpy(z21Clients[i].name, name, CLIENT_NAME_LEN);
    z21Clients[i].name[CLIENT_NAME_LEN] = '\0';
  }
}

// Contraparte de loadClientNames(): vuelca z21Clients[] a EEPROM tal cual
// esta en RAM (slots sin nombre se guardan vacios). Se llama directamente
// desde los handlers de /clients -- sin EEPROM.commit() de mas ni
// ESP.restart(), a diferencia de saveConfig().
void saveClientNamesToEeprom() {
  EEPROM.write(EEPROM_ADDR_CLIENTS_VALID, EEPROM_SECTION_VALID);
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    int base = EEPROM_ADDR_CLIENTS + i * EEPROM_CLIENT_RECORD_LEN;
    if (z21Clients[i].inUse && z21Clients[i].hasName) {
      EEPROM.write(base, z21Clients[i].keyIsMac ? 1 : 0);
      uint8_t key[6] = {0, 0, 0, 0, 0, 0};
      if (z21Clients[i].keyIsMac) {
        memcpy(key, z21Clients[i].mac, 6);
      } else {
        IPAddress ip = z21Clients[i].ip;
        key[0] = ip[0]; key[1] = ip[1]; key[2] = ip[2]; key[3] = ip[3];
      }
      writeEEPROMBytes(base + 1, key, 6);
      writeEEPROMString(base + 1 + 6, z21Clients[i].name, CLIENT_NAME_LEN);
    } else {
      EEPROM.write(base, 0);
      uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
      writeEEPROMBytes(base + 1, zero, 6);
      writeEEPROMString(base + 1 + 6, "", CLIENT_NAME_LEN);
    }
  }
  EEPROM.commit();
}

// Guarda SIEMPRE las 4 secciones juntas -- de momento el formulario del
// portal las manda todas en un solo POST a /save. Si el dia de mañana se
// separan en formularios/handlers distintos, cada seccion ya tiene su
// propio byte de validez y se podria guardar de forma independiente sin
// tocar esta funcion en bloque.
void saveConfig() {
  EEPROM.write(EEPROM_ADDR_NET_VALID, EEPROM_SECTION_VALID);
  for (uint8_t i = 0; i < WIFI_MAX_NETWORKS; i++) {
    int base = EEPROM_ADDR_NETWORKS + i * EEPROM_NET_BLOCK_LEN;
    writeEEPROMString(base, cfgSSID[i], EEPROM_ADDR_SSID_LEN);
    writeEEPROMString(base + EEPROM_ADDR_SSID_LEN, cfgPass[i], EEPROM_ADDR_PASS_LEN);
  }

  EEPROM.write(EEPROM_ADDR_WEBAUTH_VALID, EEPROM_SECTION_VALID);
  writeEEPROMString(EEPROM_ADDR_WEBUSER, cfgWebUser, EEPROM_ADDR_WEBUSER_LEN);
  writeEEPROMString(EEPROM_ADDR_WEBPASS, cfgWebPass, EEPROM_ADDR_WEBPASS_LEN);

  EEPROM.write(EEPROM_ADDR_MAC_VALID, cfgMacCustomEnabled ? EEPROM_SECTION_VALID : 0);
  writeEEPROMBytes(EEPROM_ADDR_MAC, cfgMacAddr, EEPROM_ADDR_MAC_LEN);

  EEPROM.write(EEPROM_ADDR_OTAPASS_VALID, EEPROM_SECTION_VALID);
  writeEEPROMString(EEPROM_ADDR_OTAPASS, cfgOtaPass, EEPROM_ADDR_OTAPASS_LEN);

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
  apModeSinceMs = millis(); // arranca (o reinicia) el contador para el reintento periodico
  apSSID = String(AP_SSID_PREFIX) + String(ESP.getChipId(), HEX);
  WiFi.mode(WIFI_AP);
  applyMac();
  WiFi.softAPConfig(AP_FIXED_IP, AP_GATEWAY, AP_SUBNET);
  // max_connection=8: este SI es un limite real (no arbitrario nuestro) --
  // es el maximo de estaciones WiFi que el chip ESP8266 puede tener
  // asociadas a la vez en modo AP, a nivel de SDK/hardware. El valor por
  // defecto del core si no se especifica es 4; lo subimos al maximo real.
  // Solo importa si el ESP esta haciendo de AP (fallback, sin router) --
  // en modo STA (conectado a tu router de casa, el caso normal) este
  // limite no aplica en absoluto: lo gestiona tu router, no el ESP.
  WiFi.softAP(apSSID.c_str(), AP_FIXED_PASSWORD, 1, 0, 8);
  IPAddress ip = WiFi.softAPIP();
  evLogf("[WiFi] Modo AP SSID=%s IP=%d.%d.%d.%d MAC=%s", apSSID.c_str(), ip[0], ip[1], ip[2], ip[3], WiFi.softAPmacAddress().c_str());
}

// Intenta conectar, en orden, a cada una de las hasta WIFI_MAX_NETWORKS
// redes guardadas en EEPROM. Se detiene en la primera que conecte. Si
// ninguna conecta (o no hay ninguna guardada), cae a modo AP.
//
// Se llama tanto en el arranque (setup()) como desde maybeRetrySTA() para
// reintentar en segundo plano mientras estamos en modo AP -- por eso pone
// WiFi.mode(WIFI_STA) explicitamente cada vez, para salir limpiamente del
// modo AP si veniamos de ahi.
void connectWiFi() {
  bool anySsidConfigured = false;
  for (uint8_t i = 0; i < WIFI_MAX_NETWORKS; i++) {
    if (strlen(cfgSSID[i]) > 0) { anySsidConfigured = true; break; }
  }

  if (!anySsidConfigured) {
    evLogf("[WiFi] Sin ningun SSID guardado en EEPROM -> AP directo");
    startAPFallback();
    return;
  }

  WiFi.mode(WIFI_STA);
  applyMac();

  for (uint8_t i = 0; i < WIFI_MAX_NETWORKS; i++) {
    if (strlen(cfgSSID[i]) == 0) continue; // slot vacio, se salta

    evLogf("[WiFi] Probando red %d/%d: %s", i + 1, WIFI_MAX_NETWORKS, cfgSSID[i]);
    WiFi.begin(cfgSSID[i], cfgPass[i]);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
      delay(200);
      DBG("."); // progreso visual solo por Serial, no vale la pena en el log web
    }

    if (WiFi.status() == WL_CONNECTED) {
      isAPMode = false;
      apModeSinceMs = 0; // ya no estamos en AP, se desactiva el reintento periodico
      IPAddress ip = WiFi.localIP();
      evLogf("[WiFi] Conectado a %s. IP=%d.%d.%d.%d RSSI=%d MAC=%s", cfgSSID[i], ip[0], ip[1], ip[2], ip[3], WiFi.RSSI(), WiFi.macAddress().c_str());
      return;
    }

    evLogfL(LOG_LVL_WARN, "[WiFi] Timeout conectando a %s", cfgSSID[i]);
    WiFi.disconnect(); // limpio antes de probar la siguiente red
  }

  evLogfL(LOG_LVL_WARN, "[WiFi] Ninguna de las redes guardadas disponible -> fallback a AP");
  startAPFallback();
}

// Se llama en cada vuelta de loop(). Mientras estemos en modo AP, cada
// WIFI_STA_RETRY_INTERVAL_MS reintenta conectar a alguna de las redes
// guardadas (por si volvio a estar disponible, o si al principio no se
// via porque el router aun no habia arrancado). connectWiFi() ya se
// encarga de, si falla, volver a levantar el AP -- aqui solo hay que
// decidir CUANDO reintentar.
void maybeRetrySTA() {
  if (!isAPMode || apModeSinceMs == 0) return;
  if (millis() - apModeSinceMs < WIFI_STA_RETRY_INTERVAL_MS) return;

  evLogf("[WiFi] En modo AP, reintentando conectar a alguna red guardada...");
  connectWiFi();
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
             ? "<span class='mega'>MEGA-&gt;ESP</span> "
             : "<span class='esp'>ESP-&gt;MEGA</span> ";
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
  // Se usa WiFi.SSID() (el SSID real al que estamos conectados) en vez de
  // cfgSSID directamente: ahora hay hasta 3 redes guardadas, y esta es la
  // forma mas simple de saber cual de ellas es la activa sin tener que
  // llevar un indice aparte.
  String staSSID = isAPMode ? apSSID : WiFi.SSID();
  uint8_t ssidLen = (uint8_t)staSSID.length();
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
  memcpy(&payload[idx], staSSID.c_str(), ssidLen);
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
    trackClient(lastUdpSourceIP, lastUdpSourcePort);
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
    replyQueuePush(lastUdpSourceIP, lastUdpSourcePort);

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
    } else if (megaType == FRAME_TYPE_Z21) {
      IPAddress replyIp;
      uint16_t replyPort;
      if (replyQueuePop(replyIp, replyPort)) {
        z21Udp.beginPacket(replyIp, replyPort);
        z21Udp.write(megaBuf, megaLen);
        z21Udp.endPacket();
      } else {
        // No hay ningun cliente pendiente en la cola -- puede pasar si el
        // Mega manda un frame Z21 que no es respuesta directa a nada (ver
        // limitacion de broadcasts documentada junto a la cola FIFO mas
        // arriba). Se descarta en vez de mandarlo a cualquiera.
        evLogfL(LOG_LVL_WARN, "[Z21] Frame del Mega sin cliente pendiente en la cola -- descartado");
      }

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

// ---------------------------------------------------------------------
// "Chrome" comun a todas las paginas del portal (barra de navegacion +
// apertura de <head>) -- responsabilidad unica: generar ese marco comun,
// para no repetirlo (y desincronizarlo) en cada handler por separado.
// Antes cada handler traia su propia copia literal de la barra de
// navegacion; /log, /sniffer y /test se habian quedado sin el enlace a la
// configuracion porque nadie lo actualizo a la vez en los 4 sitios.
// ---------------------------------------------------------------------
const char PAGE_NAV[] PROGMEM =
  "<nav><a href='/'>Estado</a> | <a href='/clients'>Clientes</a> | <a href='/sniffer'>Sniffer</a> | "
  "<a href='/log'>Log</a> | <a href='/test'>Prueba de enlace</a> | "
  "<a href='/#config'>Config</a></nav>";

String pageNav() {
  return FPSTR(PAGE_NAV);
}

// Apertura comun de pagina: <head> con la hoja de estilos compartida
// (/style.css, servida desde PROGMEM en gzip, ver web_assets.h), cabecera
// con el <h1>, la barra de nav, y abre el contenedor .wrap (centrado, con
// margen) donde va el contenido propio de cada pagina. Cada handler debe
// cerrar ese .wrap ademas de </body></html> -- ver pageFoot().
// extraHead es para markup adicional dentro de <head> que no todas las
// paginas necesitan (p.ej. el meta refresh de /log y /sniffer) -- se deja
// vacio por defecto.
String pageHead(const char *title, const char *h1, const char *extraHead = "") {
  String html = "<html><head><meta charset='utf-8'>";
  html += extraHead;
  html += "<link rel='stylesheet' href='/style.css'>";
  html += "<title>" + String(title) + "</title></head><body>";
  html += "<header><h1>" + String(h1) + "</h1></header>";
  html += pageNav();
  html += "<div class='wrap'>";
  return html;
}

// Cierre comun de pagina: complementa a pageHead(), cierra el .wrap que
// esta abrio.
String pageFoot() {
  return "</div></body></html>";
}

void handleStyleCss() {
  // No requiere autenticacion (checkWebAuth()): es solo CSS estatico, sin
  // datos sensibles ni de configuracion -- exigir login aqui obligaria al
  // navegador a mandar Basic Auth (o fallar) antes incluso de poder
  // pintar la pagina de login/estado, sin ganar nada a cambio.
  webServer.sendHeader("Content-Encoding", "gzip");
  webServer.sendHeader("Cache-Control", "public, max-age=86400"); // 1 dia: solo cambia con firmware nuevo
  webServer.send_P(200, "text/css", (const char *)STYLE_CSS_GZIP, STYLE_CSS_GZIP_LEN);
}

void handleAppJs() {
  // Mismo razonamiento que handleStyleCss(): estatico, sin datos
  // sensibles, no requiere autenticacion.
  webServer.sendHeader("Content-Encoding", "gzip");
  webServer.sendHeader("Cache-Control", "public, max-age=86400");
  webServer.send_P(200, "application/javascript", (const char *)APP_JS_GZIP, APP_JS_GZIP_LEN);
}

// Escapa un String para poder meterlo dentro de HTML, tanto en contenido
// de texto (<p>...</p>) como dentro de un atributo entre comillas simples
// (value='...') -- por eso escapa tambien la comilla simple, no solo & < >.
//
// Por que hace falta esto: varios valores que se reinsertan en las
// paginas del portal NO los escribe el dueño del ESP a mano -- el SSID
// que aparece en "Red conectada" o el que se guarda al pulsar "Red 1/2/3"
// en el buscador (ver /scan) es el nombre que OTRO ROUTER (de un vecino,
// por ejemplo) decidio ponerse. Sin escapar, un SSID con una comilla
// rompe el atributo value='...' del formulario, y uno con <script> se
// ejecutaria sin mas al abrir tu propio /log o / -- self-XSS, pero real.
String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// Escapa un String para poder meterlo como valor string dentro de JSON
// (comillas y backslash escapados, caracteres de control fuera). Los SSID
// normalmente no llevan nada raro, pero nada impide que alguien ponga
// comillas en el suyo -- sin esto, ese SSID rompería el JSON generado.
String jsonEscape(const String &in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c >= 0x20) {
      out += c; // los caracteres de control (<0x20) se descartan directamente
    }
  }
  return out;
}

// Escanea las redes WiFi visibles y las devuelve en JSON, para que el
// formulario de configuracion las ofrezca en una lista en vez de tener
// que escribir el SSID a mano (ver scanNetworks() en app.js / web_assets.h).
void handleScan() {
  if (!checkWebAuth()) return;

  // WiFi.scanNetworks() necesita la interfaz STA activa. Si estamos en
  // modo AP (fallback), se activa STA tambien solo para el escaneo y se
  // vuelve al modo original al terminar -- así no se deja un STA a medias
  // colgado ni se interrumpe el AP para los clientes ya conectados a el.
  WiFiMode_t prevMode = WiFi.getMode();
  if (prevMode == WIFI_AP) {
    WiFi.mode(WIFI_AP_STA);
  }

  int n = WiFi.scanNetworks();
  evLogf("[WiFi] Escaneo manual desde el portal: %d redes visibles", n);

  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"enc\":" + String(WiFi.encryptionType(i) == ENC_TYPE_NONE ? 0 : 1) + "}";
  }
  json += "]";
  WiFi.scanDelete(); // libera la memoria de los resultados del escaneo

  if (prevMode == WIFI_AP) {
    WiFi.mode(WIFI_AP); // volver al modo original, no dejar STA activo sin usarlo
  }

  webServer.send(200, "application/json", json);
}

// ---------------------------------------------------------------------
// Backup / Restore de la configuracion (redes WiFi, credenciales del
// portal, MAC personalizada) -- ver /backup y /restore mas abajo.
//
// El JSON se genera y se lee a mano (sin libreria tipo ArduinoJson) por
// el mismo motivo que /scan: es un formato PLANO y controlado enteramente
// por nosotros (lo generamos y lo consumimos los dos), asi que no hace
// falta un parser JSON general -- basta con buscar cada "clave":"valor"
// literal. Si el dia de manana el formato se complica (anidado, arrays
// variables) esto dejaria de ser suficiente y tocaria plantearse una
// libreria de verdad.
// ---------------------------------------------------------------------

// Busca "key":"valor" en json y copia valor (sin comillas, con \" y \\
// des-escapados) en outBuf. Devuelve false si la clave no aparece.
bool jsonGetString(const String &json, const char *key, char *outBuf, size_t outBufLen) {
  String pattern = "\"" + String(key) + "\":\"";
  int start = json.indexOf(pattern);
  if (start < 0) return false;
  start += pattern.length();

  size_t outIdx = 0;
  for (int i = start; i < (int)json.length() && outIdx < outBufLen - 1; i++) {
    char c = json[i];
    if (c == '"') break; // fin del valor
    if (c == '\\' && i + 1 < (int)json.length()) {
      i++;
      c = json[i]; // \" -> ", \\ -> \ (no hay otros escapes en lo que generamos nosotros)
    }
    outBuf[outIdx++] = c;
  }
  outBuf[outIdx] = '\0';
  return true;
}

// Busca "key":true o "key":false. Si la clave no aparece, devuelve defaultVal.
bool jsonGetBool(const String &json, const char *key, bool defaultVal) {
  String pattern = "\"" + String(key) + "\":";
  int start = json.indexOf(pattern);
  if (start < 0) return defaultVal;
  start += pattern.length();
  return json.startsWith("true", start);
}

// Aplica un backup ya leido en memoria a las variables de config en RAM
// (cfgSSID/cfgPass/cfgWebUser/cfgWebPass/cfgMacAddr). NO llama a
// saveConfig() -- eso lo decide quien la llama, para poder validar antes
// de comprometerse a escribir en EEPROM y reiniciar.
//
// Campos ausentes en el JSON se dejan tal cual estaban (igual que en
// handleSave(): un campo que no llega no borra lo que ya habia). Solo
// hace falta que aparezca al menos un SSID para considerar el backup
// valido -- si no, lo mas probable es que sea un archivo equivocado.
bool applyBackupJson(const String &json) {
  if (json.indexOf("\"formatVersion\":1") < 0) {
    evLogfL(LOG_LVL_ERROR, "[CFG] Restore: falta o no coincide \"formatVersion\":1 -- se ignora");
    return false;
  }

  bool anyNetworkFound = false;
  char tmpSsid[EEPROM_ADDR_SSID_LEN + 1];
  char tmpPass[EEPROM_ADDR_PASS_LEN + 1];
  for (uint8_t i = 0; i < WIFI_MAX_NETWORKS; i++) {
    String ssidKey = "ssid" + String(i + 1);
    String passKey = "pass" + String(i + 1);
    if (jsonGetString(json, ssidKey.c_str(), tmpSsid, sizeof(tmpSsid))) {
      memcpy(cfgSSID[i], tmpSsid, sizeof(tmpSsid));
      if (strlen(tmpSsid) > 0) anyNetworkFound = true;
    }
    if (jsonGetString(json, passKey.c_str(), tmpPass, sizeof(tmpPass))) {
      memcpy(cfgPass[i], tmpPass, sizeof(tmpPass));
    }
  }

  char tmpUser[EEPROM_ADDR_WEBUSER_LEN + 1];
  char tmpWebPass[EEPROM_ADDR_WEBPASS_LEN + 1];
  if (jsonGetString(json, "webUser", tmpUser, sizeof(tmpUser)) && strlen(tmpUser) > 0) {
    memcpy(cfgWebUser, tmpUser, sizeof(tmpUser));
  }
  if (jsonGetString(json, "webPass", tmpWebPass, sizeof(tmpWebPass)) && strlen(tmpWebPass) > 0) {
    memcpy(cfgWebPass, tmpWebPass, sizeof(tmpWebPass));
  }

  char tmpOtaPass[EEPROM_ADDR_OTAPASS_LEN + 1];
  if (jsonGetString(json, "otaPass", tmpOtaPass, sizeof(tmpOtaPass)) && strlen(tmpOtaPass) > 0) {
    memcpy(cfgOtaPass, tmpOtaPass, sizeof(tmpOtaPass));
  }

  if (jsonGetBool(json, "macCustomEnabled", false)) {
    char tmpMac[18];
    uint8_t parsedMac[6];
    if (jsonGetString(json, "mac", tmpMac, sizeof(tmpMac)) &&
        parseMacString(String(tmpMac), parsedMac) && isMacUnicastValid(parsedMac)) {
      memcpy(cfgMacAddr, parsedMac, EEPROM_ADDR_MAC_LEN);
      cfgMacCustomEnabled = true;
    } else {
      evLogfL(LOG_LVL_WARN, "[CFG] Restore: MAC del backup invalida -- se ignora ese campo, se mantiene la MAC actual");
    }
  } else {
    cfgMacCustomEnabled = false;
  }

  if (!anyNetworkFound) {
    evLogfL(LOG_LVL_ERROR, "[CFG] Restore: el JSON no trae ningun SSID -- probablemente no es un backup valido, se ignora entero");
  }
  return anyNetworkFound;
}

void handleBackup() {
  if (!checkWebAuth()) return;

  String json = "{";
  json += "\"formatVersion\":1,";
  json += "\"espFwVersion\":\"" + String(ESP_FW_VERSION_MAJOR) + "." + String(ESP_FW_VERSION_MINOR) + "\",";
  for (uint8_t i = 0; i < WIFI_MAX_NETWORKS; i++) {
    json += "\"ssid" + String(i + 1) + "\":\"" + jsonEscape(String(cfgSSID[i])) + "\",";
    json += "\"pass" + String(i + 1) + "\":\"" + jsonEscape(String(cfgPass[i])) + "\",";
  }
  json += "\"webUser\":\"" + jsonEscape(String(cfgWebUser)) + "\",";
  json += "\"webPass\":\"" + jsonEscape(String(cfgWebPass)) + "\",";
  json += "\"otaPass\":\"" + jsonEscape(String(cfgOtaPass)) + "\",";
  json += "\"macCustomEnabled\":" + String(cfgMacCustomEnabled ? "true" : "false") + ",";
  json += "\"mac\":\"" + (cfgMacCustomEnabled ? macToString(cfgMacAddr) : String("")) + "\"";
  json += "}";

  // El archivo lleva las passwords en claro (WiFi y del propio portal) --
  // es la unica forma de que un restore deje todo listo sin tener que
  // volver a teclearlas. Content-Disposition fuerza la descarga en vez de
  // mostrarlo en el navegador, pero el fichero en si no va cifrado:
  // tratarlo como se trataria cualquier fichero con passwords.
  webServer.sendHeader("Content-Disposition", "attachment; filename=\"z21_emulator_backup.json\"");
  webServer.send(200, "application/json", json);
  evLogf("[CFG] Backup descargado desde el portal");
}

// Handler de SUBIDA (se llama varias veces mientras llega el archivo, ver
// HTTPUpload). Solo acumula bytes en restoreUploadBuffer; el JSON se
// interpreta y se aplica despues, en handleRestoreDone(), una vez que ha
// llegado entero.
void handleRestoreUpload() {
  HTTPUpload &upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    restoreUploadBuffer = "";
    restoreUploadTooBig = false;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (restoreUploadBuffer.length() + upload.currentSize > RESTORE_MAX_UPLOAD_BYTES) {
      restoreUploadTooBig = true; // se sigue leyendo el resto para no romper el POST, pero se descarta
    } else {
      for (size_t i = 0; i < upload.currentSize; i++) {
        restoreUploadBuffer += (char)upload.buf[i];
      }
    }
  }
  // UPLOAD_FILE_END: nada que hacer aqui, se procesa todo en handleRestoreDone()
}

// Handler de FIN de peticion POST /restore (se llama una vez, despues de
// que handleRestoreUpload() ya haya recibido el archivo entero).
void handleRestoreDone() {
  if (!checkWebAuth()) return;

  if (restoreUploadTooBig) {
    evLogfL(LOG_LVL_ERROR, "[CFG] Restore: archivo demasiado grande (>%d bytes) -- se ignora", RESTORE_MAX_UPLOAD_BYTES);
    webServer.send(400, "text/html", "<html><body>Archivo demasiado grande para ser un backup valido. <a href='/#config'>Volver</a></body></html>");
    restoreUploadBuffer = "";
    return;
  }
  if (restoreUploadBuffer.length() == 0) {
    webServer.send(400, "text/html", "<html><body>No se ha recibido ningun archivo. <a href='/#config'>Volver</a></body></html>");
    return;
  }

  bool ok = applyBackupJson(restoreUploadBuffer);
  restoreUploadBuffer = ""; // liberar la RAM cuanto antes, ya no hace falta

  if (!ok) {
    webServer.send(400, "text/html", "<html><body>El archivo no tiene el formato de backup esperado (ver log del ESP para el detalle). <a href='/#config'>Volver</a></body></html>");
    return;
  }

  saveConfig();
  evLogf("[CFG] Configuracion restaurada desde backup, reiniciando...");
  webServer.send(200, "text/html", "<html><body>Restaurado. Reiniciando...</body></html>");
  delay(500);
  ESP.restart();
}

void handleRoot() {
  if (!checkWebAuth()) return;

  String html = pageHead("Z21 Emulator Debug", "Z21 Emulator - Debug");
  html += "<div class='card'>";
  html += "<h2>Estado general</h2>";
  html += "<p>Firmware ESP: v" + String(ESP_FW_VERSION_MAJOR) + "." + String(ESP_FW_VERSION_MINOR) + "</p>";
  html += "<p>Modo actual: " + String(isAPMode ? "AP (fallback)" : "STA (conectado)") + "</p>";
  if (!isAPMode) {
    html += "<p>Red conectada: " + htmlEscape(WiFi.SSID()) + "</p>";
  }
  html += "<p>IP: " + (isAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "</p>";
  html += "<p>RSSI: " + String(isAPMode ? 0 : WiFi.RSSI()) + " dBm</p>";
  html += "<p>MAC efectiva: ";
  html += (isAPMode ? WiFi.softAPmacAddress() : WiFi.macAddress());
  html += cfgMacCustomEnabled ? " (personalizada)" : " (por defecto, prefijo 84:2B:BC)";
  html += "</p>";
  html += "<p>OTA (actualizacion por WiFi): activo, hostname '" + htmlEscape(otaHostname) + "' (deberia aparecer como puerto de red en el IDE de Arduino; password propia, ver Configuraci&oacute;n)</p>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>Socket UDP Z21 (puerto " + String(Z21_UDP_PORT) + ")</h2>";
  html += "<p>Paquetes UDP vistos en total: " + String(udpPacketsSeen) + "</p>";
  if (udpPacketsSeen > 0) {
    html += "<p>Ultimo origen: " + lastUdpSourceIP.toString() + ":" + String(lastUdpSourcePort) + "</p>";
  } else {
    html += "<p class='err'>Ningun paquete UDP ha llegado nunca a este socket. Revisa que el cliente este en la misma red y apuntando a esta IP y puerto.</p>";
  }
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>Estado del Mega</h2>";
  html += "<p>Sincronizacion inicial: " + String(megaSynced ? "completada" : "en curso (esperando HELLO/SYNC_ACK)") + "</p>";
  if (isMegaOnline()) {
    html += "<p class='ok'><b>EN LINEA</b></p>";
    html += "<p>Firmware Mega: v" + String(megaFwVersionMajor) + "." + String(megaFwVersionMinor) + "</p>";
    html += "<p>Estado: " + String(statusCodeText(megaStatusCode)) + " (codigo " + String(megaStatusCode) + ")</p>";
    html += "<p>Ultimo heartbeat hace: " + String(millis() - lastHeartbeatReceivedMs) + " ms</p>";
    html += "<p>Uptime del Mega: " + String(megaUptimeMs / 1000) + " s</p>";
    html += "<p>Tiempo de ciclo medio: " + String(megaCycleAvgUs) + " us, maximo: " + String(megaCycleMaxUs) + " us</p>";
    html += "<p>RAM libre en el Mega: " + String(megaFreeRam) + " bytes</p>";
    html += "<p>Frames Z21 recibidos OK: " + String(megaFramesOk) + ", con error: " + String(megaFramesBad) + "</p>";
  } else if (lastHeartbeatReceivedMs == 0) {
    html += "<p class='err'><b>NUNCA SE HA RECIBIDO UN HEARTBEAT DEL MEGA</b></p>";
    html += "<p>Revisa: DIP switch en modo MCU&lt;-&gt;ESP, selector fisico RXD3/TXD3, baudios 115200 y cableado.</p>";
  } else {
    html += "<p class='err'><b>SIN RESPUESTA</b> (ultimo heartbeat hace " + String(millis() - lastHeartbeatReceivedMs) + " ms)</p>";
  }
  html += "</div>";

  html += "<div class='card'>";
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
  html += "</div>";

  // TODO: campo para la direccion de cliente XpressNet (pendiente de definir)
  html += "<div class='card'>";
  html += "<h2 id='config'>Configuración</h2>";
  html += "<form method='POST' action='/save'>";
  html += "<p>Hasta 3 redes WiFi, se prueban en este orden antes de caer al modo AP. Deja una fila con SSID vacio para no usar ese hueco.</p>";
  html += "<p><button type='button' onclick='scanNetworks()'>Buscar redes WiFi</button> ";
  html += "<i>(tarda unos segundos; pulsa \"Red 1/2/3\" junto a la que quieras para rellenar ese hueco)</i></p>";
  html += "<div id='scanResults'></div>";
  for (uint8_t i = 0; i < WIFI_MAX_NETWORKS; i++) {
    html += "Red " + String(i + 1) + " - SSID: <input id='ssid" + String(i + 1) + "' name='ssid" + String(i + 1) + "' value='" + htmlEscape(String(cfgSSID[i])) + "'> ";
    html += "Password: <input name='pass" + String(i + 1) + "' type='password'> ";
    html += "<i>(dejar en blanco para no cambiarla)</i><br>";
  }

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
  html += String(defTail) + "): <input name='mac' value='" + htmlEscape(macFieldValue) + "' placeholder='AA:BB:CC:DD:EE:FF'><br>";
  html += "<a href='/?genmac=1'>Generar MAC aleatoria (84:2B:BC:xx:xx:xx)</a> (revisa el campo y pulsa Guardar para aplicarla; evita colisiones con otros equipos de la red. Si quieres otro prefijo, escribelo tu directamente en el campo)<br>";

  // Usuario/password de ACCESO a este portal (checkWebAuth()). Antes no
  // habia forma de cambiarlos desde aqui -- solo existian por dentro con
  // el valor por defecto admin/z21admin, que nunca se llegaba a tocar.
  // Password en 2 campos (nueva + repetir) para no arriesgarse a quedar
  // fuera del portal por una errata al escribirla -- si no coinciden, se
  // ignora el cambio y se mantiene la anterior (ver handleSave()).
  html += "<h3>Acceso al portal</h3>";
  html += "<p>Usuario: <input name='webuser' value='" + htmlEscape(String(cfgWebUser)) + "'></p>";
  html += "<p>Password nueva: <input name='webpass' type='password'> ";
  html += "Repetir: <input name='webpass2' type='password'> ";
  html += "<i>(dejar ambas en blanco para no cambiarla)</i></p>";

  // Password de OTA (flashear firmware por WiFi), SEPARADA de la del
  // portal a proposito -- ver comentario en setupOTA(). Mismo patron de
  // 2 campos que la password del portal, por el mismo motivo.
  html += "<h3>Actualizaci&oacute;n OTA</h3>";
  html += "<p>Password nueva: <input name='otapass' type='password'> ";
  html += "Repetir: <input name='otapass2' type='password'> ";
  html += "<i>(dejar ambas en blanco para no cambiarla; es la que pide el IDE de Arduino al flashear por WiFi)</i></p>";

  html += "<input type='submit' value='Guardar y reiniciar'>";
  html += "</form>";
  html += "</div>";

  // Backup/restore en su propio <form> (no puede ir anidado dentro del de
  // arriba -- HTML no permite <form> dentro de <form>, y ademas este
  // necesita enctype='multipart/form-data' para poder subir un archivo).
  html += "<div class='card'>";
  html += "<h2>Backup / Restore</h2>";
  html += "<p>El archivo de backup incluye las passwords de las redes WiFi, del portal y de OTA <b>en claro (sin cifrar)</b> -- gu&aacute;rdalo con el mismo cuidado que una password.</p>";
  html += "<p><a href='/backup'>Descargar backup (JSON)</a></p>";
  html += "<form method='POST' action='/restore' enctype='multipart/form-data'>";
  html += "<input type='file' name='backupfile' accept='.json,application/json'> ";
  html += "<input type='submit' value='Restaurar backup'>";
  html += "<p><i>Restaurar sobrescribe las redes WiFi, las credenciales del portal y la MAC personalizada con lo que traiga el archivo, y reinicia el ESP.</i></p>";
  html += "</form>";
  html += "</div>";

  html += "<script src='/app.js'></script>";
  html += pageFoot();

  webServer.send(200, "text/html", html);
}

// Clase CSS asociada a cada nivel (ver lvl-info/lvl-warn/lvl-error en el
// style.css compartido), para que un WARN/ERROR salte a la vista en medio
// de 60 lineas de INFO sin tener que leer una a una.
const char *evLogLevelClass(uint8_t level) {
  switch (level) {
    case LOG_LVL_WARN: return "lvl-warn";
    case LOG_LVL_ERROR: return "lvl-error";
    default: return "lvl-info";
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
      // Escape minimo de comillas/backslash para que el JSON no se rompa
      // (esto es para el propio JSON, no para HTML -- ver htmlEscape() en
      // la vista normal de esta pagina para ese otro caso).
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

  String html = pageHead("Log ESP", "Log de eventos del ESP", "<meta http-equiv='refresh' content='2'>");
  html += "<div class='card'>";
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

  html += "<p class='mono'>";
  bool any = false;
  for (int i = (int)evLogCount - 1; i >= 0; i--) {
    uint8_t idx = evLogChronoIndex((uint8_t)i);
    if (evLog[idx].level < minLevel) continue;
    any = true;
    unsigned long ageMs = millis() - evLog[idx].ms;
    html += "<span class='" + String(evLogLevelClass(evLog[idx].level)) + "'>";
    html += "[+" + String(ageMs / 1000) + "." + String((ageMs % 1000) / 100) + "s] ";
    html += "[" + String(evLogLevelName(evLog[idx].level)) + "] ";
    html += htmlEscape(String(evLog[idx].text)); // ver htmlEscape(): el texto puede incluir un SSID ajeno (escaneo), no es todo generado por nosotros
    html += "</span>\n";
  }
  if (!any) {
    html += (evLogCount == 0) ? "(todavia no hay eventos registrados)" : "(ningun evento cumple el filtro de nivel actual)";
  }
  html += "</p></div>" + pageFoot();

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

  String html = pageHead("Sniffer", "Sniffer Serial ESP&lt;-&gt;Mega", "<meta http-equiv='refresh' content='2'>");
  html += "<div class='card'>";
  html += "<p>Estado: " + String(sniffOn ? "ACTIVO (capturando)" : "PAUSADO") + "</p>";
  html += "<p>Bytes crudos totales desde el arranque: " + String(totalRawBytesFromMega) + "</p>";
  html += "<p>Frames descartados por checksum de framing: " + String(framesRxChkFailFromMega) + "</p>";
  html += "<p><a href='/sniffer?on=1'>Reiniciar captura</a> | <a href='/sniffer?off=1'>Pausar</a> | ";
  html += (showRaw ? "<a href='/sniffer'>Ver decodificado</a>" : "<a href='/sniffer?raw=1'>Ver hex plano</a>");
  html += " | <a href='/'>Volver</a></p>";
  html += "</div>";

  html += "<div class='card'>";
  if (showRaw) {
    html += "<h3>Bytes crudos capturados (hex, orden cronologico, un solo sentido Mega-&gt;ESP)</h3>";
    html += "<p>Util solo para saber si hay comunicacion fisica en absoluto; para ver el protocolo, usa la vista decodificada.</p>";
    if (sniffLogCount == 0) {
      html += "<p>Todavia no ha llegado ningun byte.</p>";
    } else {
      html += "<p class='mono-inline'>";
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
    html += "<p class='mono'>";
    html += decodeSniffFrameLog(dirFilter);
    html += "</p>";
  }
  html += "</div>";
  html += pageFoot();

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

  String html = pageHead("Prueba de enlace", "Prueba de enlace Mega↔ESP");
  html += "<div class='card'>";
  html += "<p>Envía un frame de prueba al Mega para comprobar que el canal Serie funciona.</p>";
  html += "<p><a href='/test?send=1'>Enviar frame de prueba</a></p>";
  html += "<p>Último frame visto del Mega: tipo=0x" + String(lastMegaFrameType, HEX) + ", len=" + String(lastMegaFrameLen) + "</p>";
  html += "</div>";
  html += pageFoot();
  webServer.send(200, "text/html", html);
}

// ---------------------------------------------------------------------
// Panel de clientes Z21 (/clients) -- ver Z21Client / trackClient() mas
// arriba para como se rellena esta tabla.
// ---------------------------------------------------------------------
void handleClients() {
  if (!checkWebAuth()) return;

  String html = pageHead("Clientes Z21", "Clientes Z21");
  html += "<div class='card'>";
  html += "<p class='muted'>Un cliente se marca <b>Desconectado</b> si no manda nada en " + String(CLIENT_TIMEOUT_MS / 1000) + " segundos. ";
  html += "La MAC solo se puede ver cuando el ESP esta en modo AP (el cliente conectado directamente a el); en modo STA (conectado a tu router) no hay forma de obtenerla, se identifica solo por IP -- si a ese cliente le cambia la IP, se vera como un cliente nuevo distinto.</p>";
  html += "</div>";

  html += "<div class='card'>";
  bool any = false;
  html += "<table><tr><th>Estado</th><th>Nombre</th><th>IP</th><th>MAC</th><th>Visto hace</th><th>Acci&oacute;n</th></tr>";
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    if (!z21Clients[i].inUse) continue;
    any = true;
    Z21Client &c = z21Clients[i];
    bool online = (c.lastSeenMs != 0) && (millis() - c.lastSeenMs < CLIENT_TIMEOUT_MS);

    html += "<tr><td>";
    html += online ? "<span class='ok'>&#9679; Conectado</span>" : "<span class='muted'>&#9675; Desconectado</span>";
    html += "</td><td>" + (c.hasName ? htmlEscape(String(c.name)) : String("<span class='muted'>(sin nombre)</span>"));
    html += "</td><td>" + c.ip.toString();
    html += "</td><td>" + (c.keyIsMac ? macToString(c.mac) : String("<span class='muted'>N/D</span>"));
    html += "</td><td>" + (c.lastSeenMs == 0 ? String("nunca en este arranque") : String((millis() - c.lastSeenMs) / 1000) + "s");
    html += "</td><td>";
    html += "<form class='inline-form' method='POST' action='/clients/save'>";
    html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    html += "<input type='text' name='name' value='" + (c.hasName ? htmlEscape(String(c.name)) : String("")) + "' placeholder='nombre'>";
    html += "<input type='submit' value='Guardar'>";
    html += "</form>";
    if (c.hasName) {
      html += "<form class='inline-form' method='POST' action='/clients/forget'>";
      html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
      html += "<input type='submit' value='Olvidar'>";
      html += "</form>";
    }
    html += "</td></tr>";
  }
  html += "</table>";
  if (!any) {
    html += "<p class='muted'>Todav&iacute;a no se ha visto ning&uacute;n cliente Z21 en este arranque.</p>";
  }
  html += "</div>";
  html += pageFoot();

  webServer.send(200, "text/html", html);
}

void handleClientSave() {
  if (!checkWebAuth()) return;

  if (webServer.hasArg("idx")) {
    int idx = webServer.arg("idx").toInt();
    if (idx >= 0 && idx < MAX_TRACKED_CLIENTS && z21Clients[idx].inUse) {
      String name = webServer.hasArg("name") ? webServer.arg("name") : "";
      name.trim();
      if (name.length() > 0) {
        name.toCharArray(z21Clients[idx].name, CLIENT_NAME_LEN + 1);
        z21Clients[idx].hasName = true;
        saveClientNamesToEeprom();
        evLogf("[CLIENTS] Nombre asignado a cliente #%d: %s", idx, z21Clients[idx].name);
      }
    }
  }

  webServer.sendHeader("Location", "/clients");
  webServer.send(303, "text/plain", "");
}

void handleClientForget() {
  if (!checkWebAuth()) return;

  if (webServer.hasArg("idx")) {
    int idx = webServer.arg("idx").toInt();
    if (idx >= 0 && idx < MAX_TRACKED_CLIENTS && z21Clients[idx].inUse) {
      bool online = (z21Clients[idx].lastSeenMs != 0) && (millis() - z21Clients[idx].lastSeenMs < CLIENT_TIMEOUT_MS);
      z21Clients[idx].hasName = false;
      z21Clients[idx].name[0] = '\0';
      if (!online) {
        z21Clients[idx].inUse = false; // libera el hueco del todo si ya no esta conectado
      }
      saveClientNamesToEeprom();
      evLogf("[CLIENTS] Nombre olvidado para cliente #%d", idx);
    }
  }

  webServer.sendHeader("Location", "/clients");
  webServer.send(303, "text/plain", "");
}

void handleSave() {
  if (!checkWebAuth()) return;

  for (uint8_t i = 0; i < WIFI_MAX_NETWORKS; i++) {
    String ssidField = "ssid" + String(i + 1);
    String passField = "pass" + String(i + 1);
    if (webServer.hasArg(ssidField)) {
      webServer.arg(ssidField).toCharArray(cfgSSID[i], EEPROM_ADDR_SSID_LEN + 1);
    }
    // Igual que con la MAC: el campo de password llega vacio si el usuario
    // no lo toco (por seguridad no se rellena de vuelta en el formulario),
    // asi que vacio significa "mantener la que ya habia", no "borrarla".
    if (webServer.hasArg(passField) && webServer.arg(passField).length() > 0) {
      webServer.arg(passField).toCharArray(cfgPass[i], EEPROM_ADDR_PASS_LEN + 1);
    }
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

  // Usuario/password de acceso al portal. El username, igual que el resto
  // de campos, vacio = no tocar. La password requiere los 2 campos
  // iguales -- si no coinciden se ignora el cambio entero (se mantiene la
  // password anterior) para no dejar el portal con una password a medio
  // escribir y sin forma de saber cual quedo guardada.
  if (webServer.hasArg("webuser") && webServer.arg("webuser").length() > 0) {
    webServer.arg("webuser").toCharArray(cfgWebUser, EEPROM_ADDR_WEBUSER_LEN + 1);
  }
  if (webServer.hasArg("webpass") && webServer.arg("webpass").length() > 0) {
    String p1 = webServer.arg("webpass");
    String p2 = webServer.hasArg("webpass2") ? webServer.arg("webpass2") : "";
    if (p1 == p2) {
      p1.toCharArray(cfgWebPass, EEPROM_ADDR_WEBPASS_LEN + 1);
      evLogfL(LOG_LVL_WARN, "[CFG] Password del portal web actualizada");
    } else {
      evLogfL(LOG_LVL_ERROR, "[CFG] Las dos passwords del portal no coinciden -- se ignora, se mantiene la anterior");
    }
  }

  // Password de OTA -- mismo patron de doble campo, pero en su propia
  // seccion de EEPROM (ver EEPROM_ADDR_OTAPASS_VALID mas arriba).
  if (webServer.hasArg("otapass") && webServer.arg("otapass").length() > 0) {
    String p1 = webServer.arg("otapass");
    String p2 = webServer.hasArg("otapass2") ? webServer.arg("otapass2") : "";
    if (p1 == p2) {
      p1.toCharArray(cfgOtaPass, EEPROM_ADDR_OTAPASS_LEN + 1);
      evLogfL(LOG_LVL_WARN, "[CFG] Password de OTA actualizada");
    } else {
      evLogfL(LOG_LVL_ERROR, "[CFG] Las dos passwords de OTA no coinciden -- se ignora, se mantiene la anterior");
    }
  }

  saveConfig();
  evLogf("[CFG] Configuracion guardada (SSID1=%s, SSID2=%s, SSID3=%s), reiniciando...", cfgSSID[0], cfgSSID[1], cfgSSID[2]);

  webServer.send(200, "text/html", "<html><body>Guardado. Reiniciando...</body></html>");
  delay(500);
  ESP.restart();
}

// ---------------------------------------------------------------------
// OTA (actualizacion de firmware por WiFi, sin cable USB)
// ---------------------------------------------------------------------
// Estandar del core ESP8266 (ArduinoOTA): en el IDE de Arduino, con el
// ESP en la misma red, aparece como un "puerto de red" ademas del puerto
// serie de siempre -- se selecciona igual que un puerto USB y "Subir"
// manda el firmware por WiFi en vez de por cable.
//
// Password: cfgOtaPass, SEPARADA de la password del portal (ver comentario
// junto a EEPROM_ADDR_OTAPASS mas arriba) -- flashear firmware es un
// privilegio distinto a solo ver/editar la configuracion.
//
// Si el usuario cambia esta password desde /save, se relee en el
// siguiente arranque (handleSave() ya reinicia el ESP tras guardar, asi
// que no hace falta refrescar ArduinoOTA en caliente).
String otaHostname; // se rellena en setupOTA(); tambien se muestra en el estado del portal

void setupOTA() {
  otaHostname = "z21emulator-" + String(ESP.getChipId(), HEX);
  ArduinoOTA.setHostname(otaHostname.c_str());
  ArduinoOTA.setPassword(cfgOtaPass);

  ArduinoOTA.onStart([]() {
    String tipo = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "sistema de archivos (LittleFS/SPIFFS)";
    evLogf("[OTA] Actualizacion iniciada (%s) -- no apagar ni desconectar hasta que termine", tipo.c_str());
  });
  ArduinoOTA.onEnd([]() {
    evLogf("[OTA] Actualizacion completada, reiniciando...");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    const char *reason = "desconocido";
    switch (error) {
      case OTA_AUTH_ERROR: reason = "password incorrecta"; break;
      case OTA_BEGIN_ERROR: reason = "fallo al iniciar (sin espacio o flash mal particionada)"; break;
      case OTA_CONNECT_ERROR: reason = "fallo de conexion"; break;
      case OTA_RECEIVE_ERROR: reason = "fallo recibiendo datos"; break;
      case OTA_END_ERROR: reason = "fallo al finalizar la escritura en flash"; break;
    }
    evLogfL(LOG_LVL_ERROR, "[OTA] Error (%d): %s", (int)error, reason);
  });

  ArduinoOTA.begin();
  evLogf("[OTA] Listo. Hostname='%s', password propia de OTA (distinta de la del portal)", otaHostname.c_str());
}

void setupWebServer() {
  webServer.on("/", handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/sniffer", handleSniffer);
  webServer.on("/log", handleLog);
  webServer.on("/test", handleTest);
  webServer.on("/clients", handleClients);
  webServer.on("/clients/save", HTTP_POST, handleClientSave);
  webServer.on("/clients/forget", HTTP_POST, handleClientForget);
  webServer.on("/style.css", handleStyleCss);
  webServer.on("/app.js", handleAppJs);
  webServer.on("/scan", handleScan);
  webServer.on("/backup", handleBackup);
  webServer.on("/restore", HTTP_POST, handleRestoreDone, handleRestoreUpload);
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
  setupOTA();
}

void loop() {
  handleZ21Udp();
  webServer.handleClient();
  maybeRetrySTA(); // si estamos en AP fallback, reintenta STA cada WIFI_STA_RETRY_INTERVAL_MS
  ArduinoOTA.handle();
}
