#ifndef SFSHELL_BAR_H
#define SFSHELL_BAR_H

#include <gtk/gtk.h>

/* Altezza fissa della barra (px). Valore unico, imposto nel codice e
 * blindato via provider CSS ad alta priorita': il file style.css NON puo'
 * sovrascriverlo. */
#define SFSHELL_BAR_HEIGHT 32

/* Crea la top bar come layer-surface ancorata in alto sul monitor.
 * La window ritornata e' gia' configurata per gtk4-layer-shell. */
GtkWindow *bar_new(GtkApplication *app);

#endif /* SFSHELL_BAR_H */
