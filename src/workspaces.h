#ifndef ASHELL_WORKSPACES_H
#define ASHELL_WORKSPACES_H

#include <gtk/gtk.h>

/* Widget dei workspace di Hyprland: si popola dallo stato corrente e si
 * aggiorna live ascoltando il socket eventi di Hyprland. */
GtkWidget *workspaces_new(void);

#endif /* ASHELL_WORKSPACES_H */
