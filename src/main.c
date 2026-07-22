#include "bar.h"
#include "config.h"

/* CSS di default, stile "menu bar macOS": sfondo trasparente, niente pill,
 * solo testo. Il colore di stato dei workspace va sul numero stesso.
 * Alla prima esecuzione viene scritto in ~/.config/ashell/style.css, poi
 * quel file e' ricaricato a caldo ad ogni modifica: si puo' cambiare
 * qualsiasi cosa senza riavviare. */
static const char *DEFAULT_CSS =
    "/* Finestra trasparente: si vede il desktop dietro la barra. */\n"
    ".bar-window {\n"
    "  background-color: transparent;\n"
    "}\n"
    "\n"
    ".bar {\n"
    "  /* Ombra sfumata: scura in alto, si dissolve fino a trasparente in basso.\n"
    "     Non e' un layer nero uniforme, e' un degrade dall'alto verso il basso. */\n"
    "  background-image: linear-gradient(to bottom,\n"
    "                    rgba(0, 0, 0, 0.80) 0%,\n"
    "                    rgba(0, 0, 0, 0.48) 55%,\n"
    "                    transparent 100%);\n"
    "  color: @theme_fg_color;\n"
    "  padding: 0 10px;\n"
    "  /* Font stile macOS. L'altezza NON si mette qui: e' fissa nel codice. */\n"
    "  font-family: \"SF Pro Text\", \"SF Pro Display\", \"SF Pro\", sans-serif;\n"
    "}\n"
    "\n"
    ".clock {\n"
    "  color: @theme_fg_color;\n"
    "  font-weight: 500;\n"
    "  font-feature-settings: \"tnum\";\n"
    "}\n"
    "\n"
    "/* Contenitore orologio: nessuno sfondo, solo spaziatura. */\n"
    ".pill {\n"
    "  padding: 0 4px;\n"
    "}\n"
    "\n"
    "/* Workspace: solo testo, lo stato colora il numero. */\n"
    ".workspaces button.ws {\n"
    "  all: unset;\n"
    "  padding: 0 7px;\n"
    "  font-weight: 600;\n"
    "  color: alpha(@theme_fg_color, 0.55);\n"
    "}\n"
    "\n"
    "/* Vuoto (o 1..5 non ancora creato): numero smorto. */\n"
    ".workspaces button.ws.empty {\n"
    "  color: alpha(@theme_fg_color, 0.30);\n"
    "}\n"
    "\n"
    "/* Ha finestre ma non attivo: numero piu' presente. */\n"
    ".workspaces button.ws.occupied {\n"
    "  color: alpha(@theme_fg_color, 0.75);\n"
    "}\n"
    "\n"
    "/* Attivo: numero color accento del tema (blu). */\n"
    ".workspaces button.ws.active {\n"
    "  color: @theme_selected_bg_color;\n"
    "}\n"
    "\n"
    "/* Urgente: numero rosso (hook di stile, non ancora cablato all'evento). */\n"
    ".workspaces button.ws.urgent {\n"
    "  color: @error_color;\n"
    "}\n"
    "\n"
    "/* Hover leggero solo sui non-attivi/non-urgenti. */\n"
    ".workspaces button.ws:hover:not(.active):not(.urgent) {\n"
    "  color: @theme_fg_color;\n"
    "}\n"
    "\n"
    "/* ---- System tray ---- */\n"
    ".tray-toggle, .tray-item {\n"
    "  all: unset;\n"
    "  padding: 0 5px;\n"
    "  color: alpha(@theme_fg_color, 0.85);\n"
    "}\n"
    ".tray-toggle:hover, .tray-item:hover {\n"
    "  color: @theme_fg_color;\n"
    "}\n"
    "/* Freccia: ruota con animazione smooth al toggle. */\n"
    ".tray-arrow {\n"
    "  transition: -gtk-icon-transform 220ms ease;\n"
    "}\n"
    ".tray-toggle.expanded .tray-arrow {\n"
    "  -gtk-icon-transform: rotate(180deg);\n"
    "}\n"
    "\n"
    "/* Menu contestuale (dbusmenu): minimale, semitrasparente. */\n"
    "popover.tray-menu {\n"
    "  background: transparent;\n"
    "}\n"
    "popover.tray-menu > contents {\n"
    "  background-color: alpha(@theme_bg_color, 0.72);\n"
    "  color: @theme_fg_color;\n"
    "  border: 1px solid alpha(@theme_fg_color, 0.10);\n"
    "  border-radius: 10px;\n"
    "  padding: 4px;\n"
    "  box-shadow: 0 6px 20px rgba(0, 0, 0, 0.45);\n"
    "}\n"
    "popover.tray-menu button.tray-menu-item {\n"
    "  all: unset;\n"
    "  padding: 5px 14px;\n"
    "  border-radius: 6px;\n"
    "  color: @theme_fg_color;\n"
    "  transition: background-color 120ms ease;\n"
    "}\n"
    "popover.tray-menu button.tray-menu-item:hover {\n"
    "  background-color: alpha(@theme_fg_color, 0.14);\n"
    "}\n"
    "popover.tray-menu button.tray-menu-item:disabled {\n"
    "  opacity: 0.4;\n"
    "}\n"
    "popover.tray-menu separator.tray-menu-sep {\n"
    "  margin: 4px 6px;\n"
    "  min-height: 1px;\n"
    "  background-color: alpha(@theme_fg_color, 0.14);\n"
    "}\n"
    "\n"
    "/* ---- Media player ---- */\n"
    ".media-title {\n"
    "  color: @theme_fg_color;\n"
    "  font-weight: 500;\n"
    "  margin-right: 2px;\n"
    "}\n"
    "/* Tasti piccoli in Material Symbols. */\n"
    ".media-icon {\n"
    "  font-family: \"Material Symbols Rounded\";\n"
    "  font-size: 15px;\n"
    "}\n"
    ".media-btn {\n"
    "  all: unset;\n"
    "  padding: 0 1px;\n"
    "  color: alpha(@theme_fg_color, 0.85);\n"
    "}\n"
    ".media-btn:hover {\n"
    "  color: @theme_fg_color;\n"
    "}\n"
    "\n"
    "/* ---- App launcher ---- */\n"
    "/* Bottone nella barra: cerchietto pieno. */\n"
    ".launcher-btn {\n"
    "  all: unset;\n"
    "  padding: 0 6px;\n"
    "}\n"
    ".launcher-dot {\n"
    "  min-width: 12px;\n"
    "  min-height: 12px;\n"
    "  border-radius: 9999px;\n"
    "  background-color: alpha(@theme_fg_color, 0.85);\n"
    "  transition: background-color 140ms ease, transform 140ms ease;\n"
    "}\n"
    ".launcher-btn:hover .launcher-dot {\n"
    "  background-color: @theme_selected_bg_color;\n"
    "  transform: scale(1.12);\n"
    "}\n"
    "\n"
    "/* Popup overlay: velo scuro a tutto schermo. */\n"
    ".launcher-popup {\n"
    "  background-color: transparent;\n"
    "}\n"
    "/* Velo: SOLO fade in/out (nessuno scale). Il gradient a bordi sfumati\n"
    "   e' iniettato da config; il colore/opacita' da ashell.conf. */\n"
    ".launcher-backdrop {\n"
    "  background-color: transparent;\n"
    "  transition: opacity 300ms ease;\n"
    "}\n"
    ".launcher-backdrop.opening {\n"
    "  opacity: 0;\n"
    "  transition: none;\n"   /* stato iniziale istantaneo (poi fade-in) */
    "}\n"
    ".launcher-backdrop.closing {\n"
    "  opacity: 0;\n"         /* fade-out in chiusura */
    "}\n"
    "\n"
    "/* Pannello: scala dal centro mantenendo l'aspect ratio (scale uniforme),\n"
    "   veloce e smooth (easeOutCubic). */\n"
    ".launcher-panel {\n"
    "  background-color: alpha(@theme_bg_color, 0.55);\n"
    "  border-radius: 22px;\n"
    "  padding: 18px;\n"
    "  box-shadow: 0 24px 60px rgba(0, 0, 0, 0.45);\n"
    "  transform-origin: center;\n"
    "  transition: opacity 260ms cubic-bezier(0.33, 1, 0.68, 1),\n"
    "              transform 300ms cubic-bezier(0.33, 1, 0.68, 1);\n"
    "}\n"
    ".launcher-panel.opening {\n"
    "  opacity: 0;\n"
    "  transform: scale(0.5);\n"   /* piu' piccolo, aspect ratio invariato */
    "  transition: none;\n"        /* salto istantaneo, poi cresce */
    "}\n"
    ".launcher-panel.closing {\n"
    "  opacity: 0;\n"
    "  transform: scale(0.5);\n"   /* rimpicciolisce dal centro in chiusura */
    "}\n"
    "\n"
    "/* Barra di ricerca: minimale, senza bordi. */\n"
    ".launcher-search {\n"
    "  background-color: transparent;\n"
    "  color: @theme_fg_color;\n"
    "  border: none;\n"
    "  outline: none;\n"
    "  box-shadow: none;\n"
    "  min-height: 34px;\n"
    "}\n"
    ".launcher-search > text {\n"
    "  color: @theme_fg_color;\n"
    "}\n"
    "\n"
    "/* Unico separatore tra ricerca e griglia. */\n"
    ".launcher-sep {\n"
    "  background-color: alpha(@theme_fg_color, 0.15);\n"
    "  min-height: 1px;\n"
    "  margin: 2px 4px 6px 4px;\n"
    "}\n"
    "\n"
    "/* Griglia app. */\n"
    ".launcher-grid {\n"
    "  background-color: transparent;\n"
    "}\n"
    ".launcher-cell-wrap {\n"
    "  padding: 6px;\n"
    "  border-radius: 16px;\n"
    "  transition: background-color 140ms ease;\n"
    "}\n"
    ".launcher-cell-wrap:hover {\n"
    "  background-color: alpha(@theme_selected_bg_color, 0.30);\n"
    "}\n"
    ".launcher-label {\n"
    "  color: @theme_fg_color;\n"
    "  font-size: 12px;\n"
    "}\n";

static GtkCssProvider *user_provider = NULL;
static char           *user_css_path = NULL;

/* Ricarica il CSS utente dal file (o lo svuota se il file non esiste). */
static void reload_user_css(void)
{
    if (!user_provider || !user_css_path)
        return;

    if (g_file_test(user_css_path, G_FILE_TEST_EXISTS))
        gtk_css_provider_load_from_path(user_provider, user_css_path);
    else
        gtk_css_provider_load_from_string(user_provider, "");
}

static void on_config_changed(GFileMonitor *monitor G_GNUC_UNUSED,
                              GFile *file, GFile *other G_GNUC_UNUSED,
                              GFileMonitorEvent event G_GNUC_UNUSED,
                              gpointer user_data G_GNUC_UNUSED)
{
    /* Filtra i file rilevanti nella cartella monitorata. */
    char *name = g_file_get_basename(file);
    if (g_strcmp0(name, "style.css") == 0)
        reload_user_css();
    else if (g_strcmp0(name, config_basename()) == 0)
        config_reload();
    g_free(name);
}

static void setup_style(void)
{
    GdkDisplay *display = gdk_display_get_default();

    /* Base embeddata (priorita' APPLICATION): garantisce il funzionamento
     * anche se l'utente rimuove regole dal proprio file. */
    GtkCssProvider *base = gtk_css_provider_new();
    gtk_css_provider_load_from_string(base, DEFAULT_CSS);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(base),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(base);

    /* Cartella di config + file di stile editabile (creato al primo avvio). */
    char *dir = g_build_filename(g_get_user_config_dir(), "ashell", NULL);
    g_mkdir_with_parents(dir, 0755);
    user_css_path = g_build_filename(dir, "style.css", NULL);
    if (!g_file_test(user_css_path, G_FILE_TEST_EXISTS))
        g_file_set_contents(user_css_path, DEFAULT_CSS, -1, NULL);

    /* Override utente (priorita' USER, piu' alta della base). */
    user_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(user_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    reload_user_css();

    /* LOCK: altezza barra fissa a ASHELL_BAR_HEIGHT, a priorita' PIU' ALTA
     * di USER -> nessuna regola nel file style.css puo' sovrascriverla. */
    GtkCssProvider *lock = gtk_css_provider_new();
    char *lock_css = g_strdup_printf(
        ".bar { min-height: %dpx; }", ASHELL_BAR_HEIGHT);
    gtk_css_provider_load_from_string(lock, lock_css);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(lock),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
    g_free(lock_css);
    g_object_unref(lock);

    /* Hot-reload: monitora la cartella (robusto ai salvataggi con rename). */
    GFile *dir_file = g_file_new_for_path(dir);
    GFileMonitor *monitor =
        g_file_monitor_directory(dir_file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (monitor)
        g_signal_connect(monitor, "changed",
                         G_CALLBACK(on_config_changed), NULL);
    /* monitor resta vivo per tutta la durata dell'app. */
    g_object_unref(dir_file);
    g_free(dir);
}

static void on_activate(GtkApplication *app, gpointer user_data)
{
    (void) user_data;
    config_init();
    setup_style();
    bar_new(app);
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("dev.ashell.bar",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
