#ifndef SFSHELL_WORKSPACES_H
#define SFSHELL_WORKSPACES_H

#include <gtk/gtk.h>

/* Widget dei workspace di Hyprland: si popola dallo stato corrente e si
 * aggiorna live ascoltando il socket eventi di Hyprland. */
GtkWidget *workspaces_new(void);

#endif /* SFSHELL_WORKSPACES_H */
