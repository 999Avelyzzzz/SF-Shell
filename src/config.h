#pragma once
#include <gtk/gtk.h>

/* Configurazione di AShell da ~/.config/ashell/ashell.conf (key = value).
 * Al primo avvio scrive un file di default commentato. */

/* Scrive il default se manca, carica e applica la configurazione. */
void config_init(void);

/* Ricarica dal file e riapplica (hot-reload). */
void config_reload(void);

/* Nome del file di config (per il filtro del monitor): "ashell.conf". */
const char *config_basename(void);
