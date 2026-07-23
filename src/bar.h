#ifndef SFSHELL_BAR_H
#define SFSHELL_BAR_H

#include <gtk/gtk.h>

/* Altezza fissa della barra (px). Valore unico, imposto nel codice e
 * blindato via provider CSS ad alta priorita'. */
#define SFSHELL_BAR_HEIGHT 32

/* Coda d'ombra sotto la barra (px): area extra, NON riservata (non entra
 * nella exclusive zone), su cui il gradiente si dissolve dolcemente sopra il
 * desktop, evitando lo stacco netto al bordo inferiore della barra. */
#define SFSHELL_BAR_SHADOW 8

/* Crea la top bar come layer-surface ancorata in alto sul monitor indicato.
 * `monitor` fissa l'output su cui compare la barra (NULL = lascia scegliere al
 * compositor). La window ritornata e' gia' configurata per gtk4-layer-shell. */
GtkWindow *bar_new(GtkApplication *app, GdkMonitor *monitor);

#endif /* SFSHELL_BAR_H */
