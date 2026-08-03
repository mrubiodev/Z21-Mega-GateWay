# Z21 Mega GateWay

Emulador del protocolo Z21 (Roco/Fleischmann) sobre una placa combo **Mega +WiFi R3 (ATmega2560 + ESP8266)**, funcionando como esclavo XpressNet de una **Roco MultiMaus** standalone (con booster propio), con roadmap hacia LocoNet y central Z21 completa.

## Estado actual

En desarrollo — fase 1 (esclavo XpressNet, reconocimiento como Z21 legítima ante la app oficial).

## Estructura del repositorio

```
z21-mega-emulator/
├── README.md          — este archivo
├── AGENT.md           — contexto y convenciones para agentes de IA que trabajen en este repo
├── docs/
│   └── Z21_EMULATOR_SPEC.md   — especificación funcional y de hardware
└── src/
│   ├── esp8266_wifi/  — sketch del ESP8266: WiFi, servidor web, auth, sniffer WebSocket
│   ├── mega_z21/      — sketch del Mega: núcleo Z21, XpressNet, pantalla, encoder, E-stop
│   └── shared/        — z21_protocol.h, constantes compartidas (copiar a mano, ver AGENT.md)
└── gerber/            — archivos Gerber para la fabricación de la PCB / shield adaptador(Aun no disponibles)
```

## Configuración de Hardware (Placa RobotDyn / HW-888)

Para permitir que el ATmega2560 y el ESP8266 se comuniquen internamente mientras mantienes el puerto USB libre para monitorizar la depuración desde el PC:

| Modo de Operación | DIP Switches (1-8) | Selector SW2 | Uso / Función |
| :--- | :--- | :--- | :--- |
| **Cargar código al ESP8266** | `OFF OFF OFF OFF ON ON ON OFF` | Indiferente | Conecta el USB directamente al ESP8266 |
| **Cargar código al Mega** | `OFF OFF ON ON OFF OFF OFF OFF` | Indiferente | Conecta el USB directamente al ATmega2560 |
| **Ejecución + Depuración USB** | `ON ON ON ON OFF OFF OFF OFF` | **RX3 / TX3** | ESP8266 $\leftrightarrow$ ATmega2560 (`Serial3` @ 115200 bauds)<br>USB $\leftrightarrow$ ATmega2560 (`Serial` @ 115200 bauds) |

## Hardware

- Placa combo **Mega + WiFi R3 ATmega2560 + ESP8266** (32Mb flash, USB CH340G)
- Shield **3.5" TFT LCD** (bus paralelo de 8 bits, slot microSD) montado sobre el Mega
- Encoder rotativo con pulsador + botón de parada de emergencia dedicado (E-stop)
- Central existente: **Roco MultiMaus** standalone (XpressNet maestra, booster propio)
- Satélites WiFi futuros: Wemos D1 mini + pantalla táctil Lolin TFT-2.4

## Ver también

- [`docs/Z21_EMULATOR_SPEC.md`](docs/Z21_EMULATOR_SPEC.md) — spec completo: protocolo, reparto de hardware, roadmap de fases y backends
- [`AGENT.md`](AGENT.md) — guía para agentes de IA que continúen el desarrollo
