#include "wallpaper.h"
#include "config.h"
#include <gtk4-layer-shell.h>

/* ========================================================================
 *  Wallpaper engine. Per ogni monitor apriamo una finestra layer-shell sul
 *  layer BACKGROUND (sotto a tutto), ancorata ai quattro lati con exclusive
 *  zone -1 (copre l'intero output, ignorando la barra) e una GtkPicture in
 *  content-fit COVER (l'immagine riempie e ritaglia = "filled").
 *
 *  Percorso per monitor: prima "wallpaper-<CONNECTOR>" (es. wallpaper-DP-1),
 *  poi il generico "wallpaper". Reagisce all'hotplug dei monitor.
 * ======================================================================== */

typedef struct {
    GtkWidget *win;
    GtkWidget *pic;
} WpWin;

static GHashTable *g_wins;      /* connector (char*) -> WpWin*   */
static GListModel *g_monitors;  /* di proprieta' del display     */

/* Percorso configurato per il connector dato (specifico > generico). */
static const char *wp_path_for(const char *connector)
{
    char *key = g_strconcat("wallpaper-", connector, NULL);
    const char *p = config_get(key);
    g_free(key);
    if (!p)
        p = config_get("wallpaper");
    return p;
}

/* Crea (nascosta) la finestra di sfondo per un monitor. */
static WpWin *wp_create(GdkMonitor *mon)
{
    WpWin *w = g_new0(WpWin, 1);

    w->win = gtk_window_new();
    gtk_widget_add_css_class(w->win, "wallpaper");

    gtk_layer_init_for_window(GTK_WINDOW(w->win));
    gtk_layer_set_layer(GTK_WINDOW(w->win), GTK_LAYER_SHELL_LAYER_BACKGROUND);
    gtk_layer_set_namespace(GTK_WINDOW(w->win), "sfshell-wallpaper");
    gtk_layer_set_monitor(GTK_WINDOW(w->win), mon);
    gtk_layer_set_anchor(GTK_WINDOW(w->win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(w->win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(w->win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(w->win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    /* -1 = ignora le exclusive zone altrui e copre tutto l'output. */
    gtk_layer_set_exclusive_zone(GTK_WINDOW(w->win), -1);

    w->pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(w->pic), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(w->pic), TRUE);
    gtk_window_set_child(GTK_WINDOW(w->win), w->pic);

    return w;
}

static void wp_free(gpointer data)
{
    WpWin *w = data;
    gtk_window_destroy(GTK_WINDOW(w->win));
    g_free(w);
}

/* Ricrea/aggiorna gli sfondi per l'insieme di monitor attuale. */
static void wp_apply(void)
{
    if (!g_monitors)
        return;

    GHashTable *seen =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    guint n = g_list_model_get_n_items(g_monitors);
    for (guint i = 0; i < n; i++) {
        GdkMonitor *mon = g_list_model_get_item(g_monitors, i);  /* +1 ref */
        const char *conn = gdk_monitor_get_connector(mon);
        if (conn && gdk_monitor_is_valid(mon)) {
            WpWin *w = g_hash_table_lookup(g_wins, conn);
            if (!w) {
                w = wp_create(mon);
                g_hash_table_insert(g_wins, g_strdup(conn), w);
            } else {
                gtk_layer_set_monitor(GTK_WINDOW(w->win), mon);
            }

            const char *path = wp_path_for(conn);
            if (path && *path) {
                gtk_picture_set_filename(GTK_PICTURE(w->pic), path);
                gtk_widget_set_visible(w->win, TRUE);
            } else {
                gtk_widget_set_visible(w->win, FALSE);
            }
            g_hash_table_add(seen, g_strdup(conn));
        }
        g_object_unref(mon);
    }

    /* Elimina le finestre dei monitor scomparsi. */
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, g_wins);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        if (!g_hash_table_contains(seen, key))
            g_hash_table_iter_remove(&it);  /* wp_free via value-destroy */
    }

    g_hash_table_destroy(seen);
}

static void on_monitors_changed(GListModel *m G_GNUC_UNUSED,
                                guint position G_GNUC_UNUSED,
                                guint removed G_GNUC_UNUSED,
                                guint added G_GNUC_UNUSED,
                                gpointer ud G_GNUC_UNUSED)
{
    wp_apply();
}

void wallpaper_init(void)
{
    GdkDisplay *dpy = gdk_display_get_default();
    if (!dpy)
        return;

    g_wins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, wp_free);
    g_monitors = gdk_display_get_monitors(dpy);   /* di proprieta' del display */
    g_signal_connect(g_monitors, "items-changed",
                     G_CALLBACK(on_monitors_changed), NULL);

    wp_apply();
}

void wallpaper_reload(void)
{
    wp_apply();
}
