/*
 * loco_state.h
 * ------------
 * Definición de DummyLocoState, separada de mega_z21.ino a propósito.
 *
 * MOTIVO (no mover el struct de vuelta al .ino sin releer esto): el IDE
 * de Arduino genera automáticamente los prototipos de todas las
 * funciones del sketch e los inserta como bloque justo después de los
 * #include, en la cabecera del fichero concatenado — ANTES de cualquier
 * struct que esté definido más abajo dentro del propio .ino. Si
 * DummyLocoState viviera en mega_z21.ino, los prototipos autogenerados
 * de funciones como findOrAllocLoco(DummyLocoState*) fallarían al
 * compilar con "'DummyLocoState' does not name a type", porque en el
 * punto donde se insertan esos prototipos el struct todavía no existe.
 * Al vivir en este header, incluido al principio de mega_z21.ino, el
 * struct ya está visible antes de que el IDE inserte los prototipos.
 */
#ifndef LOCO_STATE_H
#define LOCO_STATE_H

#include <stdint.h>

// Ver TODO real en mega_z21.ino (handleXGetLocoInfo/handleXSetLocoDriveOrFunction):
// tabla de estado de locomotoras en RAM, se pierde al reiniciar, sin
// backend XpressNet real todavía.
struct DummyLocoState {
  uint16_t address;  // 0 = slot libre
  uint8_t stepsCode; // DB2 de LAN_X_LOCO_INFO: 0=14, 2=28, 4=128 pasos
  uint8_t speedByte; // DB3 tal cual: bit7=sentido (1=adelante), bits0-6=velocidad
  uint8_t f0to4;     // DB4: orden especial L=F0(bit4) F=F4(bit3) G=F3(bit2) H=F2(bit1) J=F1(bit0), ver PDF 4.4
  uint8_t f5to12;    // DB5: F5 es bit0 (LSB)
  uint8_t f13to20;   // DB6: F13 es bit0 (LSB)
  uint8_t f21to28;   // DB7: F21 es bit0 (LSB)
};

#endif // LOCO_STATE_H
