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
    "# --- Launcher: velo scuro dietro il launcher ---\n"
    "# overlay_color: colore del velo, qualsiasi colore CSS (nome o #rrggbb).\n"
    "# overlay_opacity: quanto e' trasparente, 0.0 (invisibile) .. 1.0 (pieno).\n"
    "overlay_color = #000000\n"
    "overlay_opacity = 0.35\n"
    "\n"
    "# --- Wallpaper ---\n"
    "# wallpaper: percorso dell'immagine di sfondo. Viene riempita (filled/\n"
    "# cover, ritagliando l'eccedenza) su TUTTI i monitor.\n"
    "# Per dare uno sfondo diverso a un monitor specifico usa il nome del\n"
    "# connector (es. DP-1, HDMI-A-1, eDP-1): wallpaper-DP-1 = /path/img.png\n"
    "#\n"
    "# wallpaper-extended: UNA immagine panoramica (larga) distribuita in modo\n"
    "# continuo su TUTTI i monitor. La posizione dei monitor viene letta da\n"
    "# Hyprland, quindi ogni schermo mostra la sua porzione dell'immagine:\n"
    "# ideale per i setup a doppio monitor. Un eventuale wallpaper-<CONNECTOR>\n"
    "# specifico ha comunque la precedenza sul singolo monitor.\n"
    "#\n"
    "# Priorita' per monitor: wallpaper-<CONNECTOR> > wallpaper-extended >\n"
    "# wallpaper.\n"
    "wallpaper =\n"
    "wallpaper-extended =\n";

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
 * Calcoliamo l'rgba() in C (GdkRGBA) invece di affidarci alla funzione
 * CSS alpha(), che con certi valori non produceva il velo. */
static void apply_overlay(void)
{
    if (!overlay_provider)
        return;
    const char *color   = conf_get("overlay_color", "#000000");
    const char *opacity = conf_get("overlay_opacity", "0.35");

    GdkRGBA rgba;
    if (!gdk_rgba_parse(&rgba, color))
        gdk_rgba_parse(&rgba, "#000000");
    rgba.alpha = CLAMP(g_ascii_strtod(opacity, NULL), 0.0, 1.0);

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
