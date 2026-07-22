#pragma once

/* Generatore di palette dai wallpaper. Se in config "generate_colors = true",
 * analizza i wallpaper in uso ed estrae una palette GTK coerente (moderna,
 * poco satura, professionale), che sovrascrive i colori di GTK2/3/4:
 *   ~/.config/gtk-4.0/gtk.css, ~/.config/gtk-3.0/gtk.css, ~/.gtkrc-2.0.
 *
 * Un solo wallpaper (wallpaper = path)  -> palette da quell'immagine.
 * Piu' wallpaper (per-monitor diversi)  -> colori comuni a tutti; se non ce
 *                                          ne sono, una fusione di tutti.
 * color_scheme = auto|dark|light decide il tono (auto = luminosita' media). */

/* Genera la palette (se abilitata) all'avvio. */
void colors_init(void);

/* Rigenera se la config o i wallpaper sono cambiati (hot-reload). */
void colors_reload(void);
