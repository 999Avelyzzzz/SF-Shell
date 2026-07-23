#ifndef SFSHELL_BATTERY_H
#define SFSHELL_BATTERY_H

#include <gtk/gtk.h>

/* Modulo batteria: icona a pillola con il livello di carica come riempimento
 * (piena/vuota in base alla percentuale) e il numero % disegnato dentro.
 * Legge da /sys/class/power_supply. Su sistemi senza batteria (desktop) il
 * widget resta nascosto e non occupa spazio nella barra. */
GtkWidget *battery_new(void);

#endif /* SFSHELL_BATTERY_H */
