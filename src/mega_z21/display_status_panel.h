/*
 * display_status_panel.h
 * -----------------------
 * Pinta la cabecera de estado (zona `layout.header`): enlace con el ESP,
 * datos de red, contadores de frames y estado global de salud del
 * firmware. Es la traducción visual directa de la sección "Diagnóstico y
 * watchdog" de AGENT.md — pensada para poder ver de un vistazo si el
 * gateway está listo, sin necesidad de conectar el puerto serie de debug.
 *
 * Historial de cambios:
 *   - v0.1 (2026-08-02): Creación inicial. 1 fila de título (estática) +
 *     4 filas de estado con redibujado selectivo (solo la fila cuyo
 *     valor ha cambiado desde la última llamada, para evitar parpadeo).
 */

#ifndef DISPLAY_STATUS_PANEL_H
#define DISPLAY_STATUS_PANEL_H

#include "display_layout.h"
#include "display_types.h"

// Pinta el marco fijo de la cabecera: título, versión de firmware y las
// etiquetas de cada fila (que nunca cambian). Se llama una única vez
// desde displayInit(). También deja preparado el layout interno de filas
// que usará displayStatusPanelUpdate().
void displayStatusPanelInit(const DisplayLayout &layout);

// Compara `snap` contra el último snapshot pintado y redibuja SOLO los
// campos cuyo valor haya cambiado (comparación por valor, no por
// contenido de pantalla). Pensado para llamarse periódicamente (ver
// STATUS_REFRESH_INTERVAL_MS en display_manager.cpp), no en cada vuelta
// de loop() sin más.
void displayStatusPanelUpdate(const DisplayStatusSnapshot &snap);

#endif // DISPLAY_STATUS_PANEL_H
