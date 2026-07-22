#ifndef SFSHELL_FULLSCREEN_H
#define SFSHELL_FULLSCREEN_H

#include <gtk/gtk.h>

/* Nasconde la barra quando la finestra attiva e' in fullscreen "vero" (mode 2)
 * e la rimostra quando si esce. Necessario perche' la barra vive sul layer
 * OVERLAY (sopra le finestre): senza questo, resterebbe visibile davanti a un
 * app a schermo intero. Si aggancia agli eventi di Hyprland (socket2).
 *
 * `bar` e' la window ritornata da bar_new(); il watcher segue il suo ciclo di
 * vita (si libera da solo quando la barra viene distrutta). */
void fullscreen_watch(GtkWindow *bar);

#endif /* SFSHELL_FULLSCREEN_H */
