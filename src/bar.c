#include "bar.h"
#include "workspaces.h"
#include "tray.h"
#include "media.h"
#include "launcher.h"
#include <gtk4-layer-shell.h>

/* ---- Clock -------------------------------------------------------------- */

/* Formatta data e ora nel layout "Sat Jul 11 19:31":
 * giorno-settimana e mese abbreviati (in inglese, a prescindere dalla locale),
 * giorno senza zero iniziale, ora 24h. */
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

    char *out = g_strdup_printf("%s %s %d %02d:%02d",
                                wday, month, day, hour, minute);

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

GtkWindow *bar_new(GtkApplication *app)
{
    GtkWidget *win = gtk_application_window_new(app);
    gtk_widget_add_css_class(win, "bar-window");

    /* Layer shell: barra ancorata in alto, riservando spazio (exclusive). */
    gtk_layer_init_for_window(GTK_WINDOW(win));
    /* OVERLAY (sopra il velo del launcher, che sta su TOP): cosi' il velo
     * scuro passa ANCHE sotto la barra e la barra resta visibile sopra, con
     * la sua ombra che si stacca sul velo. */
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(win), "sfshell-bar");
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_auto_exclusive_zone_enable(GTK_WINDOW(win));

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

    /* Zona destra: tray, poi orologio (a destra). */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(right), tray_new());
    gtk_box_append(GTK_BOX(right), build_clock_pill());
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(bar), right);

    gtk_window_set_child(GTK_WINDOW(win), bar);
    gtk_window_present(GTK_WINDOW(win));

    return GTK_WINDOW(win);
}
