#ifndef SFSHELL_FULLSCREEN_H
#define SFSHELL_FULLSCREEN_H

#include <gtk/gtk.h>

/* Nasconde una barra quando, SUL SUO MONITOR, la finestra a schermo intero
 * "vero" (mode 2) occupa il workspace attivo; la rimostra quando si esce.
 * Necessario perche' la barra vive sul layer OVERLAY (sopra le finestre):
 * senza questo resterebbe visibile davanti a un'app a schermo intero. La
 * valutazione e' per-monitor, quindi un fullscreen su un output non tocca la
 * barra degli altri. Si aggancia agli eventi di Hyprland (socket2).
 *
 * `bar` e' la window ritornata da bar_new(); `connector` e' il nome
 * dell'output GDK (es. "DP-1"), che combacia col nome monitor di Hyprland. Il
 * watcher e' unico e condiviso: ogni barra segue il proprio ciclo di vita e si
 * de-registra da sola quando viene distrutta. */
void fullscreen_register(GtkWindow *bar, const char *connector);

#endif /* SFSHELL_FULLSCREEN_H */
