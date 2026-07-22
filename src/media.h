#ifndef ASHELL_MEDIA_H
#define ASHELL_MEDIA_H

#include <gtk/gtk.h>

/* Modulo media player (MPRIS via DBus): nome del brano + tasti
 * indietro / play-pausa / avanti. Si nasconde se non c'e' nessun player. */
GtkWidget *media_new(void);

#endif /* ASHELL_MEDIA_H */
