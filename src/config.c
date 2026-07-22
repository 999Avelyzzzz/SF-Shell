#include "config.h"
#include <string.h>

/* ========================================================================
 *  Config file ~/.config/sfshell/sfshell.conf, formato "chiave = valore"
 *  (righe vuote e '#' = commento). Chiavi:
 *    icon_theme       icon pack per le icone delle app
 *    overlay_color    colore del velo dietro il launcher (nome CSS o #rrggbb)
 *    overlay_opacity  opacita' del velo, 0.0 (invisibile) .. 1.0 (pieno)
 *  Tutto e' hot-reload: config_reload() rilegge l'intero file e riapplica
 *  ogni valore; il GFileMonitor su main.c la richiama ad ogni modifica.
 * ======================================================================== */

#define CONF_BASENAME "sfshell.conf"

static const char *CONF_DEFAULT =
    "# Configurazione di SFShell\n"
    "#\n"
    "# icon_theme: nome dell'icon pack (icon theme) per le icone delle app,\n"
    "# es. Papirus, WhiteSur, breeze, Adwaita. Deve corrispondere a una\n"
    "# cartella in /usr/share/icons o ~/.local/share/icons (o ~/.icons).\n"
    "# Se un'icona manca nel pack scelto, fallback automatico ai temi\n"
    "# ereditati e infine a hicolor. Lascia vuoto per il tema di sistema.\n"
    "icon_theme =\n"
    "\n"
    "# --- Orologio ---\n"
    "# clock_format: 24h (es. 19:31) oppure 12h (es. 7:31 PM). Default 24h.\n"
    "# clock_ampm: stile del suffisso mattino/pomeriggio, usato solo con 12h.\n"
    "# Valori: am/pm | AM/PM | a.m/p.m | A.M/P.M. Default AM/PM. (Il testo e'\n"
    "# preso alla lettera: la parte prima di '/' e' il mattino, quella dopo il\n"
    "# pomeriggio, quindi puoi anche inventarne di tuoi, es. AM./PM.)\n"
    "clock_format = 24h\n"
    "clock_ampm = AM/PM\n"
    "\n"
    "# --- Wallpaper ---\n"
    "# Tre modi di impostare lo sfondo (priorita' per monitor:\n"
    "#   wallpaper-<CONNECTOR>  >  wallpaper-extended  >  wallpaper).\n"
    "# Di default e' attivo solo 'wallpaper'; gli altri sono esempi commentati.\n"
    "#\n"
    "# wallpaper: UNA immagine riempita (cover, ritaglia l'eccedenza) su TUTTI\n"
    "# i monitor. E' l'impostazione base.\n"
    "wallpaper = /percorso/della/tua/immagine.jpg\n"
    "#\n"
    "# wallpaper-extended: UNA immagine panoramica (larga) distribuita in modo\n"
    "# continuo su TUTTI i monitor. Le posizioni dei monitor sono lette da\n"
    "# Hyprland: ogni schermo mostra la sua porzione. Ideale per il dual monitor.\n"
    "# wallpaper-extended = /percorso/panorama.jpg\n"
    "#\n"
    "# wallpaper-<CONNECTOR>: sfondo dedicato a un monitor specifico, indicato\n"
    "# col nome del connector (es. DP-1, HDMI-A-1, eDP-1). Ha la precedenza.\n"
    "# wallpaper-DP-1 = /percorso/monitor1.jpg\n"
    "# wallpaper-HDMI-A-1 = /percorso/monitor2.jpg\n"
    "\n"
    "# --- Generazione palette dai wallpaper ---\n"
    "# generate_colors: se true, la shell analizza i wallpaper in uso e genera\n"
    "# una palette GTK coerente (moderna, poco satura, professionale),\n"
    "# SOVRASCRIVENDO i colori in ~/.config/gtk-4.0/gtk.css, ~/.config/\n"
    "# gtk-3.0/gtk.css e ~/.gtkrc-2.0. Se un file pre-esistente non e' stato\n"
    "# generato da sfshell, la prima volta ne viene salvato un .sfshell-backup.\n"
    "# Con un solo wallpaper la palette nasce da quello; con piu' wallpaper si\n"
    "# usano i colori comuni a tutti, o in mancanza una fusione di tutti.\n"
    "# color_scheme: auto|dark|light (auto = deciso dalla luminosita' media).\n"
    "generate_colors = false\n"
    "color_scheme = auto\n";

static char           *conf_path;
static char           *default_icon_theme;   /* tema di sistema, catturato */
static GtkCssProvider *overlay_provider;      /* CSS del velo (hot)         */
static GHashTable     *conf_values;           /* chiave -> valore           */

const char *config_basename(void)
{
    return CONF_BASENAME;
}

const char *config_get(const char *key)
{
    if (conf_values) {
        const char *v = g_hash_table_lookup(conf_values, key);
        if (v && *v)
            return v;
    }
    return NULL;
}

char **config_keys_with_prefix(const char *prefix)
{
    GPtrArray *a = g_ptr_array_new();
    if (conf_values) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, conf_values);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            if (g_str_has_prefix((const char *) k, prefix))
                g_ptr_array_add(a, g_strdup((const char *) k));
        }
    }
    g_ptr_array_add(a, NULL);
    return (char **) g_ptr_array_free(a, FALSE);
}

/* Valore di una chiave, o fallback se assente/vuota. */
static const char *conf_get(const char *key, const char *fallback)
{
    if (conf_values) {
        const char *v = g_hash_table_lookup(conf_values, key);
        if (v && *v)
            return v;
    }
    return fallback;
}

/* Rilegge il file in conf_values (sostituendo la tabella precedente). */
static void parse_file(void)
{
    if (conf_values)
        g_hash_table_destroy(conf_values);
    conf_values = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, g_free);

    char *content = NULL;
    if (!conf_path || !g_file_get_contents(conf_path, &content, NULL, NULL))
        return;

    char **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (*line == '#' || *line == '\0')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = g_strstrip(line);
        char *val = g_strstrip(eq + 1);
        if (*key)
            g_hash_table_insert(conf_values, g_strdup(key), g_strdup(val));
    }
    g_strfreev(lines);
    g_free(content);
}

/* Icon theme via GtkSettings ("gtk-icon-theme-name"): l'icon theme del
 * display e' un singleton read-only, set_theme_name su di esso fallisce.
 * Cambiare la setting propaga a TUTTO il display e ridisegna le icone.
 * Vuoto => ripristina il tema di sistema catturato all'init. */
static void apply_icon_theme(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;
    const char *name = conf_get("icon_theme", NULL);
    GtkSettings *settings = gtk_settings_get_for_display(display);
    g_object_set(settings, "gtk-icon-theme-name",
                 (name && *name) ? name : default_icon_theme, NULL);
}

/* Velo del launcher: colore + opacita' -> CSS su .launcher-backdrop.
 * Valori FISSI (non configurabili): il velo fa parte del look della shell.
 * Calcoliamo l'rgba() in C (GdkRGBA) invece di affidarci alla funzione
 * CSS alpha(), che con certi valori non produceva il velo. */
static void apply_overlay(void)
{
    if (!overlay_provider)
        return;

    GdkRGBA rgba;
    gdk_rgba_parse(&rgba, "#000000");
    rgba.alpha = 0.35;

    /* Bordi sfumati: non un rettangolo netto ma un radial-gradient ellittico
     * pieno al centro che sfuma a trasparente verso i bordi. In espansione
     * appare come una macchia morbida che cresce dal centro. */
    char *col = gdk_rgba_to_string(&rgba);      /* es. "rgba(0,0,0,0.55)" */
    char *css = g_strdup_printf(
        ".launcher-backdrop {"
        " background-color: transparent;"
        " background-image: radial-gradient(ellipse at center,"
        " %s 0%%, %s 55%%, transparent 100%%); }",
        col, col);
    gtk_css_provider_load_from_string(overlay_provider, css);
    g_free(col);
    g_free(css);
}

void config_reload(void)
{
    parse_file();
    apply_icon_theme();
    apply_overlay();
}

void config_init(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "sfshell", NULL);
    g_mkdir_with_parents(dir, 0755);
    conf_path = g_build_filename(dir, CONF_BASENAME, NULL);
    if (!g_file_test(conf_path, G_FILE_TEST_EXISTS))
        g_file_set_contents(conf_path, CONF_DEFAULT, -1, NULL);
    g_free(dir);

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        /* Tema di sistema PRIMA di qualsiasi override, per il ripristino. */
        g_object_get(gtk_settings_get_for_display(display),
                     "gtk-icon-theme-name", &default_icon_theme, NULL);
        /* Provider dedicato al velo, sopra style.css (USER) cosi' vince. */
        overlay_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(overlay_provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER + 2);
    }

    config_reload();
}
