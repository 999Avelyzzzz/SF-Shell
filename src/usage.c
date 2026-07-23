#include "usage.h"
#include <stdlib.h>

/* ========================================================================
 *  Cache d'uso delle app (persistente) per l'ordinamento del launcher.
 *
 *  Formato file (una riga per app usata, campi separati da spazio):
 *      <count> <last_used_epoch> <app_id>
 *  L'app_id e' l'ultimo campo: puo' contenere spazi (limite di split = 3).
 *
 *  Percorso: $XDG_CACHE_HOME/sfshell/usage (di norma ~/.cache/sfshell/usage).
 *
 *  Scritture: coalizzate con un timer (SAVE_DELAY_MS) e atomiche
 *  (g_file_set_contents scrive un temporaneo e fa rename), cosi' anche molti
 *  lanci ravvicinati costano un solo write e il file non si corrompe mai.
 * ======================================================================== */

#define SAVE_DELAY_MS 1000

typedef struct {
    int    count;
    gint64 last;      /* epoch, secondi */
} Stat;

static GHashTable *u_table;      /* app_id (string) -> Stat*     */
static char       *u_path;       /* percorso del file cache      */
static guint       u_save_timer; /* debounce del salvataggio     */
static gboolean    u_loaded;

static gint64 now_seconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

/* ---- Caricamento -------------------------------------------------------- */

static void load_file(void)
{
    char *content = NULL;
    if (!u_path || !g_file_get_contents(u_path, &content, NULL, NULL))
        return;

    char **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i])
            continue;
        /* limite 3: l'id (ultimo campo) resta intero anche con spazi. */
        char **f = g_strsplit(lines[i], " ", 3);
        if (f[0] && f[1] && f[2] && *f[2]) {
            Stat *s = g_new0(Stat, 1);
            s->count = atoi(f[0]);
            s->last  = g_ascii_strtoll(f[1], NULL, 10);
            if (s->count > 0)
                g_hash_table_insert(u_table, g_strdup(f[2]), s);
            else
                g_free(s);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(content);
}

void usage_init(void)
{
    if (u_loaded)
        return;
    u_loaded = TRUE;

    u_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    char *dir = g_build_filename(g_get_user_cache_dir(), "sfshell", NULL);
    g_mkdir_with_parents(dir, 0755);
    u_path = g_build_filename(dir, "usage", NULL);
    g_free(dir);

    load_file();
}

/* ---- Salvataggio (debounced, atomico) ----------------------------------- */

static gboolean save_cb(gpointer data G_GNUC_UNUSED)
{
    u_save_timer = 0;
    if (!u_table || !u_path)
        return G_SOURCE_REMOVE;

    GString *out = g_string_new(NULL);
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, u_table);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        Stat *s = val;
        g_string_append_printf(out, "%d %" G_GINT64_FORMAT " %s\n",
                               s->count, s->last, (const char *) key);
    }
    g_file_set_contents(u_path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
    return G_SOURCE_REMOVE;
}

static void schedule_save(void)
{
    if (!u_save_timer)
        u_save_timer = g_timeout_add(SAVE_DELAY_MS, save_cb, NULL);
}

/* ---- API ---------------------------------------------------------------- */

void usage_get(const char *app_id, int *count, gint64 *last_used)
{
    Stat *s = (u_table && app_id)
        ? g_hash_table_lookup(u_table, app_id) : NULL;
    if (count)     *count     = s ? s->count : 0;
    if (last_used) *last_used = s ? s->last  : 0;
}

void usage_bump(const char *app_id)
{
    if (!u_table || !app_id || !*app_id)
        return;
    Stat *s = g_hash_table_lookup(u_table, app_id);
    if (!s) {
        s = g_new0(Stat, 1);
        g_hash_table_insert(u_table, g_strdup(app_id), s);
    }
    s->count++;
    s->last = now_seconds();
    schedule_save();
}
