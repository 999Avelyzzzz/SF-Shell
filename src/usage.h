#ifndef SFSHELL_USAGE_H
#define SFSHELL_USAGE_H

#include <glib.h>

/* Cache persistente dell'uso delle app, usata per ordinare il launcher.
 * Chiave = app id (di norma l'id del .desktop). L'ordine di default e'
 * alfabetico: un'app mai lanciata ha conteggio 0 e resta in coda A-Z; man mano
 * che viene usata sale (per frequenza, poi per recency).
 *
 * Implementazione: hash in memoria caricato una volta all'avvio (lookup O(1)),
 * scritture su file debounced e atomiche (solo le app effettivamente usate). */

/* Carica la cache dal disco (idempotente: la prima chiamata fa il lavoro). */
void usage_init(void);

/* Conteggio lanci e ultimo uso (epoch, secondi) di un'app. 0 se sconosciuta. */
void usage_get(const char *app_id, int *count, gint64 *last_used);

/* Registra un lancio: +1 al conteggio, aggiorna il timestamp, pianifica il
 * salvataggio su disco (coalizzato). */
void usage_bump(const char *app_id);

#endif /* SFSHELL_USAGE_H */
