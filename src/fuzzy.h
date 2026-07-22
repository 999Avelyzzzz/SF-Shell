#pragma once

/* Ricerca "intelligente" tollerante agli errori di battitura.
 * Porting in C di hyprland-material-you/hypryou/utils_cy/levenshtein.pyx:
 * distanza di Levenshtein + punteggio di similarita' con partial-ratio,
 * bonus per prefisso comune e penalita' sulla differenza di lunghezza. */

/* Distanza di edit (Levenshtein) tra due stringhe, byte per byte. */
int fuzzy_levenshtein(const char *s1, const char *s2);

/* Similarita' [0,1] tra la proprieta' dell'app (s1) e il pattern di ricerca
 * (s2). 1.0 = identiche; valori alti anche con qualche lettera sbagliata.
 * Equivalente a compute_score() del progetto di riferimento. */
double fuzzy_score(const char *s1, const char *s2);
