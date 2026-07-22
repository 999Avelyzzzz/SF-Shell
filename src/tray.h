#ifndef ASHELL_TRAY_H
#define ASHELL_TRAY_H

#include <gtk/gtk.h>

/* System tray (StatusNotifierItem / KDE SNI + DBusMenu).
 * Ritorna un widget composito: [freccia toggle][revealer con le icone].
 * La freccia ruota con animazione e mostra/nasconde le icone in tray. */
GtkWidget *tray_new(void);

#endif /* ASHELL_TRAY_H */
