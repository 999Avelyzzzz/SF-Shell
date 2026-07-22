#include "fuzzy.h"

#include <stdlib.h>
#include <string.h>

/* ========================================================================
 *  Porting in C del punteggio fuzzy di hyprland-material-you (levenshtein.pyx).
 *  Tutto lavora sui byte: i pattern di ricerca e i nomi delle app sono
 *  normalizzati a minuscolo prima di arrivare qui, quindi per l'ASCII (il
 *  caso normale) il confronto byte-per-byte coincide con quello per carattere.
 * ======================================================================== */

/* Distanza di Levenshtein su a[0..la) e b[0..lb) (due righe, come nel .pyx). */
static int lev_n(const char *a, int la, const char *b, int lb)
{
    if (la == 0) return lb;
    if (lb == 0) return la;

    /* Tiene la stringa piu' corta come "b": meno memoria per le righe. */
    if (lb > la) {
        const char *ts = a; a = b; b = ts;
        int ti = la; la = lb; lb = ti;
    }

    int *prev = malloc((size_t)(lb + 1) * sizeof(int));
    int *curr = malloc((size_t)(lb + 1) * sizeof(int));
    if (!prev || !curr) { free(prev); free(curr); return (la > lb) ? la : lb; }

    for (int j = 0; j <= lb; j++)
        prev[j] = j;

    for (int i = 1; i <= la; i++) {
        curr[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int tmp = prev[j] + 1;
            if (curr[j - 1] + 1 < tmp) tmp = curr[j - 1] + 1;
            if (prev[j - 1] + cost < tmp) tmp = prev[j - 1] + cost;
            curr[j] = tmp;
        }
        int *swap = prev; prev = curr; curr = swap;
    }

    int result = prev[lb];
    free(prev);
    free(curr);
    return result;
}

int fuzzy_levenshtein(const char *s1, const char *s2)
{
    return lev_n(s1, (int) strlen(s1), s2, (int) strlen(s2));
}

/* Migliore corrispondenza di `short_s` fatta scorrere dentro `long_s`:
 * per ogni finestra di lunghezza len_s misura 1 - dist/len_s e tiene il max. */
static double partial_ratio(const char *short_s, int len_s,
                            const char *long_s, int len_l)
{
    if (len_s == 0)
        return 1.0;

    double best = 0.0;
    for (int i = 0; i + len_s <= len_l; i++) {
        int dist = lev_n(short_s, len_s, long_s + i, len_s);
        double score = 1.0 - (double) dist / len_s;
        if (score > best)
            best = score;
    }
    return best;
}

double fuzzy_score(const char *s1, const char *s2)
{
    if (strcmp(s1, s2) == 0)
        return 1.0;

    int len1 = (int) strlen(s1);
    int len2 = (int) strlen(s2);
    int max_len = (len1 > len2) ? len1 : len2;
    if (max_len == 0)
        return 1.0;

    int dist = lev_n(s1, len1, s2, len2);
    double full = 1.0 - (double) dist / max_len;

    double part = 0.0;
    if (len1 < len2)
        part = partial_ratio(s1, len1, s2, len2);
    else if (len2 < len1)
        part = partial_ratio(s2, len2, s1, len1);

    double a = part * 0.75, b = full * 0.6;
    double score = (a > b) ? a : b;

    if (len1 > 0 && len2 > 0 && s1[0] != s2[0])
        score -= 0.1;

    int len_diff = (len1 > len2) ? (len1 - len2) : (len2 - len1);
    if (len_diff >= 1)
        score -= 0.035 * len_diff / max_len;

    int min_len = (len1 < len2) ? len1 : len2;
    int common_prefix = 0;
    for (int i = 0; i < min_len; i++) {
        if (s1[i] == s2[i]) common_prefix++;
        else break;
    }
    score += 0.02 * common_prefix;

    if (score > 1.0) score = 1.0;
    else if (score < 0.0) score = 0.0;
    return score;
}
