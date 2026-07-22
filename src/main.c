#include "bar.h"
#include "colors.h"
#include "config.h"
#include "fonts.h"
#include "fullscreen.h"
#include "launcher.h"
#include "wallpaper.h"

#include <string.h>

/* CSS di default, stile "menu bar macOS": sfondo trasparente, niente pill,
 * solo testo. Il colore di stato dei workspace va sul numero stesso.
 * Alla prima esecuzione viene scritto in ~/.config/sfshell/style.css, poi
 * quel file e' ricaricato a caldo ad ogni modifica: si puo' cambiare
 * qualsiasi cosa senza riavviare. */
static const char *DEFAULT_CSS =
    "/* Finestra trasparente: si vede il desktop dietro la barra. */\n"
    ".bar-window {\n"
    "  background-color: transparent;\n"
    "}\n"
    "\n"
    "/* Backdrop: copre barra (32px) + coda d'ombra (40px). Il gradiente scorre\n"
    "   su tutta l'altezza, cosi' l'ombra ha spazio per dissolversi in modo\n"
    "   molto morbido sopra il desktop. Colore SEMPRE nero, a prescindere dalla\n"
    "   palette generata dai wallpaper: il testo mantiene lo stesso contrasto. */\n"
    ".bar-backdrop {\n"
    "  background-image: linear-gradient(to bottom,\n"
    "                    rgba(0, 0, 0, 0.85) 0%,\n"
    "                    rgba(0, 0, 0, 0.60) 20%,\n"
    "                    rgba(0, 0, 0, 0.32) 40%,\n"
    "                    rgba(0, 0, 0, 0.14) 60%,\n"
    "                    rgba(0, 0, 0, 0.05) 80%,\n"
    "                    rgba(0, 0, 0, 0.00) 100%);\n"
    "}\n"
    "\n"
    ".bar {\n"
    "  background-color: transparent;\n"
    "  color: @theme_fg_color;\n"
    "  /* Ombra sul testo: stacca le label dallo sfondo del wallpaper. */\n"
    "  text-shadow: 0 1px 3px rgba(0, 0, 0, 0.85);\n"
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
    "/* Launcher aperto: pallino sempre nel colore active dominante,\n"
    "   a prescindere dall'hover (stato .active messo dal toggle). */\n"
    ".launcher-btn.active .launcher-dot {\n"
    "  background-color: @theme_selected_bg_color;\n"
    "  transform: scale(1.12);\n"
    "}\n"
    "\n"
    "/* Popup overlay: velo scuro a tutto schermo. */\n"
    ".launcher-popup {\n"
    "  background-color: transparent;\n"
    "}\n"
    "/* Velo: SOLO fade in/out (nessuno scale). Il gradient a bordi sfumati\n"
    "   e' iniettato da config; il colore/opacita' da sfshell.conf. */\n"
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
    "/* Sfondo sfocato del launcher: il wallpaper sfocato a GPU con il filtro\n"
    "   nativo di GTK4 (nessun compositor). transform: scale spinge fuori i bordi\n"
    "   sfumati dal blur, cosi' non si vedono aloni trasparenti ai lati. Fa un\n"
    "   fade in/out sincronizzato col velo. */\n"
    ".launcher-blur-bg {\n"
    "  filter: blur(30px);\n"
    "  transform: scale(1.08);\n"
    "  transition: opacity 300ms ease;\n"
    "}\n"
    ".launcher-blur-bg.opening {\n"
    "  opacity: 0;\n"
    "  transition: none;\n"
    "}\n"
    ".launcher-blur-bg.closing {\n"
    "  opacity: 0;\n"
    "}\n"
    "\n"
    "/* Pannello: scala dal centro mantenendo l'aspect ratio (scale uniforme),\n"
    "   veloce e smooth (easeOutCubic). */\n"
    ".launcher-panel {\n"
    "  background-color: alpha(@theme_bg_color, 0.82);\n"
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

static void on_config_changed(GFileMonitor *monitor G_GNUC_UNUSED,
                              GFile *file, GFile *other G_GNUC_UNUSED,
                              GFileMonitorEvent event G_GNUC_UNUSED,
                              gpointer user_data G_GNUC_UNUSED)
{
    /* Lo stile della barra e' embeddato e non modificabile: qui reagiamo solo
     * alle modifiche del file di configurazione. */
    char *name = g_file_get_basename(file);
    if (g_strcmp0(name, config_basename()) == 0) {
        config_reload();
        wallpaper_reload();     /* riapplica gli sfondi se sono cambiati */
        colors_reload();        /* rigenera la palette se serve           */
    }
    g_free(name);
}

static void setup_style(void)
{
    GdkDisplay *display = gdk_display_get_default();

    /* Stile della barra: embeddato e NON modificabile dall'utente. */
    GtkCssProvider *base = gtk_css_provider_new();
    gtk_css_provider_load_from_string(base, DEFAULT_CSS);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(base),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(base);

    /* Cartella di config (per il file di configurazione, non lo stile). */
    char *dir = g_build_filename(g_get_user_config_dir(), "sfshell", NULL);
    g_mkdir_with_parents(dir, 0755);

    /* LOCK: altezza barra fissa a SFSHELL_BAR_HEIGHT. */
    GtkCssProvider *lock = gtk_css_provider_new();
    char *lock_css = g_strdup_printf(
        ".bar { min-height: %dpx; }", SFSHELL_BAR_HEIGHT);
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

/* Costruisce la shell (barra + wallpaper + palette). Chiamata una sola volta
 * nell'istanza primaria. */
static gboolean shell_up = FALSE;

static void build_shell(GtkApplication *app)
{
    if (shell_up)
        return;
    config_init();
    setup_style();
    colors_init();          /* palette dai wallpaper (se abilitata) */
    wallpaper_init();
    GtkWindow *bar = bar_new(app);
    fullscreen_watch(bar);  /* nasconde la barra sotto app fullscreen */
    shell_up = TRUE;
}

static void print_help(const char *argv0)
{
    g_print(
        "SFShell — barra/shell per Hyprland (Wayland, gtk4-layer-shell)\n"
        "\n"
        "Uso:\n"
        "  %s <comando>\n"
        "\n"
        "Comandi:\n"
        "  run        Avvia la shell (barra, wallpaper, launcher).\n"
        "  launcher   Apre/chiude il launcher nella shell in esecuzione,\n"
        "             comodo da bindare in Hyprland.\n"
        "  help       Mostra questo aiuto.\n"
        "\n"
        "Esempio (Hyprland):\n"
        "  exec-once = %s run\n"
        "  bind = SUPER, Space, exec, %s launcher\n",
        argv0, argv0, argv0);
}

/* Gestisce ogni invocazione (locale o inoltrata dall'istanza remota) tramite
 * la command line: cosi' `sfshell launcher` lanciato da un secondo processo
 * viene inoltrato alla shell gia' in esecuzione, che apre il launcher. */
static int on_command_line(GApplication *app, GApplicationCommandLine *cl,
                           gpointer user_data G_GNUC_UNUSED)
{
    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    const char *cmd = (argc >= 2) ? argv[1] : "run";

    int status = 0;
    if (g_strcmp0(cmd, "launcher") == 0) {
        if (!shell_up) {
            /* Nessuna shell in esecuzione: questa invocazione e' diventata lei
             * stessa l'istanza primaria. Non avviamo una shell "fantasma". */
            g_application_command_line_printerr(cl,
                "sfshell: la shell non e' in esecuzione. "
                "Avviala con 'sfshell run'.\n");
            status = 1;
        } else {
            launcher_toggle();
        }
    } else {
        /* "run" (o default): avvia la shell se non gia' su. */
        build_shell(GTK_APPLICATION(app));
    }

    g_strfreev(argv);
    g_application_command_line_set_exit_status(cl, status);
    return status;
}

int main(int argc, char **argv)
{
    const char *cmd = (argc >= 2) ? argv[1] : NULL;

    /* Nessun argomento o help -> stampa l'aiuto e basta (non avvia nulla). */
    if (!cmd || strcmp(cmd, "help") == 0 ||
        strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_help(argv[0]);
        return 0;
    }

    if (strcmp(cmd, "run") != 0 && strcmp(cmd, "launcher") != 0) {
        g_printerr("sfshell: comando sconosciuto '%s'\n\n", cmd);
        print_help(argv[0]);
        return 2;
    }

    /* Prima di GTK/Pango: restringe fontconfig ai soli font locali. */
    fonts_init();

    /* HANDLES_COMMAND_LINE: le invocazioni successive vengono inoltrate
     * all'istanza primaria (la shell), che decide cosa fare del comando. */
    GtkApplication *app = gtk_application_new("dev.sfshell.bar",
                                              G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
