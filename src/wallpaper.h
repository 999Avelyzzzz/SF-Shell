#pragma once
#include <gtk/gtk.h>

/* Engine dei wallpaper: una finestra layer-shell sul layer BACKGROUND per
 * ogni monitor, con l'immagine riempita (cover). Il percorso arriva dalla
 * config: "wallpaper = path" per tutti i monitor, "wallpaper-<CONNECTOR> =
 * path" (es. wallpaper-DP-1) per il singolo monitor (ha priorita'). */

/* Crea gli sfondi per i monitor attuali e resta in ascolto sull'hotplug. */
void wallpaper_init(void);

/* Riapplica i percorsi dalla config (hot-reload). */
void wallpaper_reload(void);
