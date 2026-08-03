/*
 * traction_config.h
 * ------------------
 * Selector de backend de tracción, EN SU PROPIO FICHERO a propósito.
 *
 * MOTIVO: el IDE/arduino-cli compila cada .cpp de la carpeta del sketch
 * como una unidad de traducción INDEPENDIENTE — un #define hecho dentro
 * de mega_z21.ino no es visible en traction_backend_xpressnet.cpp. Si
 * TRACTION_BACKEND_SELECTED viviera solo en el .ino, traction_backend_
 * xpressnet.cpp se compilaría SIEMPRE (incluyendo su `#include
 * <XpressNet.h>` de la librería externa) aunque se hubiera elegido el
 * backend dummy — rompiendo justo el caso de uso que ese backend existe
 * para cubrir: poder compilar y probar el handshake Z21 (AGENT.md
 * prioridad nº1) SIN tener la librería externa instalada ni el RS485
 * cableado. Al vivir aquí, tanto mega_z21.ino como traction_backend_
 * xpressnet.cpp/.h incluyen este mismo header y ven el mismo valor.
 *
 * Cambiar de backend: tocar SOLO la línea de TRACTION_BACKEND_SELECTED.
 */
#ifndef TRACTION_CONFIG_H
#define TRACTION_CONFIG_H

#define TRACTION_BACKEND_DUMMY 0
#define TRACTION_BACKEND_XPRESSNET 1

// ---------------------------------------------------------------------
// >>> CAMBIAR AQUÍ para elegir backend <<<
// ---------------------------------------------------------------------
#define TRACTION_BACKEND_SELECTED TRACTION_BACKEND_DUMMY

#endif // TRACTION_CONFIG_H
