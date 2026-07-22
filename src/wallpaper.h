#pragma once
#include <gtk/gtk.h>

/* Engine dei wallpaper: una finestra layer-shell sul layer BACKGROUND per
 * ogni monitor, con l'immagine riempita (cover). Il percorso arriva dalla
 * config, in ordine di priorita' per ogni monitor:
 *   wallpaper-<CONNECTOR> = path  (es. wallpaper-DP-1) immagine dedicata;
 *   wallpaper-extended    = path  UNA immagine panoramica continua spalmata
 *                                 su tutti i monitor (legge il layout da
 *                                 Hyprland: utile per setup multi-monitor);
 *   wallpaper             = path  immagine generica su tutti i monitor. */

/* Crea gli sfondi per i monitor attuali e resta in ascolto sull'hotplug. */
void wallpaper_init(void);

/* Riapplica i percorsi dalla config (hot-reload). */
void wallpaper_reload(void);
