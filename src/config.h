#pragma once
#include <gtk/gtk.h>

/* Configurazione di SFShell da ~/.config/sfshell/sfshell.conf (key = value).
 * Al primo avvio scrive un file di default commentato. */

/* Scrive il default se manca, carica e applica la configurazione. */
void config_init(void);

/* Ricarica dal file e riapplica (hot-reload). */
void config_reload(void);

/* Nome del file di config (per il filtro del monitor): "sfshell.conf". */
const char *config_basename(void);

/* Valore grezzo di una chiave, o NULL se assente/vuota. Il puntatore e'
 * valido finche' non avviene un config_reload(). */
const char *config_get(const char *key);

/* Array NULL-terminato (g_strfreev) delle chiavi che iniziano con prefix. */
char **config_keys_with_prefix(const char *prefix);
