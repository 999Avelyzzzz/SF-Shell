#include "bar.h"
#include "config.h"
#include "workspaces.h"
#include "tray.h"
#include "media.h"
#include "battery.h"
#include "launcher.h"
#include <gtk4-layer-shell.h>

/* ---- Clock -------------------------------------------------------------- */

/* Formatta data e ora nel layout "Sat Jul 11 19:31" (24h) oppure
 * "Sat Jul 11 7:31 PM" (12h). Giorno-settimana e mese abbreviati (in inglese,
 * a prescindere dalla locale), giorno senza zero iniziale.
 * Config (sfshell.conf, hot-reload):
 *   clock_format = 24h | 12h          (default 24h)
 *   clock_ampm   = am/pm | AM/PM | a.m/p.m | A.M/P.M  (solo 12h, default AM/PM)
 * Il suffisso am/pm e' preso alla lettera dalla chiave: la parte prima di '/'
 * per il mattino, quella dopo per il pomeriggio (qualsiasi stile funziona). */
static char *clock_format_now(void)
{
    static const char *const wdays[]  = { "Mon", "Tue", "Wed", "Thu",
                                          "Fri", "Sat", "Sun" };
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec" };

    GDateTime *now = g_date_time_new_now_local();

    const char *wday  = wdays[g_date_time_get_day_of_week(now) - 1]; /* 1=Mon */
    const char *month = months[g_date_time_get_month(now) - 1];      /* 1=Jan */
    int day    = g_date_time_get_day_of_month(now);
    int hour   = g_date_time_get_hour(now);
    int minute = g_date_time_get_minute(now);

    char *out;
    if (g_strcmp0(config_get("clock_format"), "12h") == 0) {
        int h12 = hour % 12;
        if (h12 == 0) h12 = 12;

        const char *style = config_get("clock_ampm");
        if (!style) style = "AM/PM";
        char **parts = g_strsplit(style, "/", 2);
        gboolean valid = parts[0] && parts[1];
        const char *suffix = (hour >= 12)
            ? (valid ? parts[1] : "PM")
            : (valid ? parts[0] : "AM");

        out = g_strdup_printf("%s %s %d %d:%02d %s",
                              wday, month, day, h12, minute, suffix);
        g_strfreev(parts);
    } else {
        out = g_strdup_printf("%s %s %d %02d:%02d",
                              wday, month, day, hour, minute);
    }

    g_date_time_unref(now);
    return out;
}

static void clock_refresh(GtkLabel *label)
{
    char *text = clock_format_now();
    gtk_label_set_text(label, text);
    g_free(text);
}

static gboolean clock_tick(gpointer user_data)
{
    clock_refresh(GTK_LABEL(user_data));
    return G_SOURCE_CONTINUE;
}

/* ---- Layout ------------------------------------------------------------- */

/* Costruisce la pill di destra con dentro l'orologio. */
static GtkWidget *build_clock_pill(void)
{
    GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(pill, "pill");
    gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);

    GtkWidget *clock = gtk_label_new(NULL);
    gtk_widget_add_css_class(clock, "clock");
    clock_refresh(GTK_LABEL(clock));

    gtk_box_append(GTK_BOX(pill), clock);

    /* Aggiorna ogni secondo: GTK ridisegna solo quando il testo cambia. */
    guint id = g_timeout_add_seconds(1, clock_tick, clock);
    /* Lega la sorgente al ciclo di vita del label. */
    g_object_set_data_full(G_OBJECT(clock), "clock-timer",
                           GUINT_TO_POINTER(id),
                           (GDestroyNotify) NULL);
    g_signal_connect_swapped(clock, "destroy",
                             G_CALLBACK(g_source_remove),
                             GUINT_TO_POINTER(id));

    return pill;
}

/* ---- Bar ---------------------------------------------------------------- */

GtkWindow *bar_new(GtkApplication *app, GdkMonitor *monitor)
{
    GtkWidget *win = gtk_application_window_new(app);
    gtk_widget_add_css_class(win, "bar-window");

    /* Layer shell: barra ancorata in alto, riservando spazio (exclusive). */
    gtk_layer_init_for_window(GTK_WINDOW(win));
    /* Fissa l'output: senza questo il compositor sceglierebbe lui il monitor,
     * mettendo tutte le barre sullo stesso. */
    if (monitor)
        gtk_layer_set_monitor(GTK_WINDOW(win), monitor);
    /* OVERLAY (sopra il velo del launcher, che sta su TOP): cosi' il velo
     * scuro passa ANCHE sotto la barra e la barra resta visibile sopra, con
     * la sua ombra che si stacca sul velo. */
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(win), "sfshell-bar");
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    /* Riserva SOLO l'altezza della barra: la coda d'ombra sotto sconfina sul
     * desktop senza rubargli spazio. */
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), SFSHELL_BAR_HEIGHT);

    /* Contenitore che porta il gradiente su tutta l'altezza (barra + coda):
     * cosi' l'ombra ha spazio per dissolversi in modo morbido. */
    GtkWidget *backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(backdrop, "bar-backdrop");

    /* Tre zone (sinistra / centro / destra): per ora usiamo solo la destra. */
    GtkWidget *bar = gtk_center_box_new();
    gtk_widget_add_css_class(bar, "bar");
    /* Altezza fissa imposta nel codice (rinforzata dal provider CSS lock). */
    gtk_widget_set_size_request(bar, -1, SFSHELL_BAR_HEIGHT);
    gtk_widget_set_vexpand(bar, FALSE);

    /* Zona sinistra: bottone launcher + media player. */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(left), launcher_button_new());
    gtk_box_append(GTK_BOX(left), media_new());
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(bar), left);

    gtk_center_box_set_center_widget(GTK_CENTER_BOX(bar), workspaces_new());

    /* Zona destra: tray, batteria, poi orologio (a destra). */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(right), tray_new());
    gtk_box_append(GTK_BOX(right), battery_new());
    gtk_box_append(GTK_BOX(right), build_clock_pill());
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(bar), right);

    /* Barra in cima al backdrop, poi la coda d'ombra (spaziatore) che estende
     * il gradiente verso il basso sopra il desktop. */
    gtk_box_append(GTK_BOX(backdrop), bar);
    GtkWidget *shadow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(shadow, "bar-shadow");
    gtk_widget_set_size_request(shadow, -1, SFSHELL_BAR_SHADOW);
    gtk_box_append(GTK_BOX(backdrop), shadow);

    gtk_window_set_child(GTK_WINDOW(win), backdrop);
    gtk_window_present(GTK_WINDOW(win));

    return GTK_WINDOW(win);
}
