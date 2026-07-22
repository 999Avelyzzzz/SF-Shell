#pragma once
#include <gtk/gtk.h>

/* Bottone con icona "cerchietto" nella zona sinistra della barra.
 * Al click apre/chiude un popup overlay centrato: barra di ricerca +
 * griglia 6x4 delle applicazioni (stile Launchpad macOS). */
GtkWidget *launcher_button_new(void);

/* Apre/chiude il launcher a livello di codice (stesso comportamento del
 * click sul bottone). Usato dal sottocomando `sfshell launcher`, cosi' lo
 * si puo' bindare in Hyprland. Va chiamato con la shell gia' avviata. */
void launcher_toggle(void);
