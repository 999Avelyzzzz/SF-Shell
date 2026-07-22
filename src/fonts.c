#include "fonts.h"

#include <fontconfig/fontconfig.h>
#include <glib.h>

/* Cartella dati installata (impostata da 'make install' via -D). Fallback
 * al percorso standard se compilato senza definirla. */
#ifndef SFSHELL_DATADIR
#define SFSHELL_DATADIR "/usr/share/sfshell"
#endif

/* Ritorna la cartella dei font locali. Stringa da liberare con g_free(). */
static char *find_fonts_dir(void)
{
    /* Override esplicito via ambiente, comodo per test o installazione. */
    const char *env = g_getenv("SFSHELL_FONTS_DIR");
    if (env && *env)
        return g_strdup(env);

    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    g_free(exe);

    /* Candidati in ordine: eseguito in-place dalla root del progetto
     * (<exedir>/src/fonts), font accanto al binario (<exedir>/fonts),
     * installato di sistema (SFSHELL_DATADIR/fonts). */
    char *cands[] = {
        g_build_filename(dir, "src", "fonts", NULL),
        g_build_filename(dir, "fonts", NULL),
        g_build_filename(SFSHELL_DATADIR, "fonts", NULL),
    };
    g_free(dir);

    char *found = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(cands); i++) {
        if (!found && g_file_test(cands[i], G_FILE_TEST_IS_DIR))
            found = g_strdup(cands[i]);
        g_free(cands[i]);
    }

    /* Nessuno esiste: ritorna il percorso installato (per il warning). */
    return found ? found : g_build_filename(SFSHELL_DATADIR, "fonts", NULL);
}

void fonts_init(void)
{
    char *fonts_dir = find_fonts_dir();

    if (!g_file_test(fonts_dir, G_FILE_TEST_IS_DIR)) {
        g_warning("sfshell: cartella font non trovata (%s); "
                  "uso i font di sistema", fonts_dir);
        g_free(fonts_dir);
        return;
    }

    /* Config vuota: non contiene alcuna <dir> di sistema, quindi fontconfig
     * NON scandira' /usr/share/fonts ne' le cartelle utente. Vi aggiungiamo
     * come uniche sorgenti i font applicativi locali. */
    FcConfig *cfg = FcConfigCreate();
    if (!cfg) {
        g_warning("sfshell: FcConfigCreate() fallita; uso i font di sistema");
        g_free(fonts_dir);
        return;
    }

    /* Una config vuota non ha cachedir: le diamo una cartella scrivibile
     * (sotto la cache utente) per evitare warning e riscansioni ad ogni
     * avvio. E' l'unica <dir> che aggiungiamo: nessun font di sistema. */
    char *cache_dir =
        g_build_filename(g_get_user_cache_dir(), "sfshell", "fontconfig", NULL);
    g_mkdir_with_parents(cache_dir, 0755);
    char *xml = g_strdup_printf(
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n"
        "<fontconfig><cachedir>%s</cachedir></fontconfig>\n", cache_dir);
    FcConfigParseAndLoadFromMemory(cfg, (const FcChar8 *) xml, FcTrue);
    g_free(xml);
    g_free(cache_dir);

    if (!FcConfigAppFontAddDir(cfg, (const FcChar8 *) fonts_dir))
        g_warning("sfshell: impossibile caricare i font da %s", fonts_dir);

    /* Rende attiva questa config: prende un riferimento a cfg e distrugge
     * la config di default precedente. Il nostro riferimento resta valido
     * per tutta la durata del processo (rilascio non necessario). */
    if (!FcConfigSetCurrent(cfg))
        g_warning("sfshell: FcConfigSetCurrent() fallita; font di sistema");

    g_free(fonts_dir);
}
