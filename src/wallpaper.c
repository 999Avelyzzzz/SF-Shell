#include "wallpaper.h"
#include "config.h"
#include <gtk4-layer-shell.h>
#include <json-glib/json-glib.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <math.h>

/* ========================================================================
 *  Wallpaper engine. Per ogni monitor apriamo una finestra layer-shell sul
 *  layer BACKGROUND (sotto a tutto), ancorata ai quattro lati con exclusive
 *  zone -1 (copre l'intero output, ignorando la barra) e una GtkPicture in
 *  content-fit COVER (l'immagine riempie e ritaglia = "filled").
 *
 *  Priorita' del contenuto, per ogni monitor:
 *    1) "wallpaper-<CONNECTOR>" (es. wallpaper-DP-1): immagine dedicata (cover).
 *    2) "wallpaper-extended"   : UNA immagine panoramica spalmata su TUTTI i
 *       monitor. Leggiamo da Hyprland (j/monitors) posizione/dimensione logica
 *       di ogni monitor e ad ognuno assegnamo il suo ritaglio dell'immagine,
 *       cosi' il panorama e' continuo su un setup multi-monitor.
 *    3) "wallpaper"            : immagine generica su tutti i monitor (cover).
 *  Reagisce all'hotplug dei monitor.
 * ======================================================================== */

typedef struct {
    GtkWidget *win;
    GtkWidget *pic;
} WpWin;

/* Rettangolo logico (post-scale) di un monitor nel layout del compositor. */
typedef struct {
    int x, y, w, h;
} MonRect;

static GHashTable *g_wins;      /* connector (char*) -> WpWin*   */
static GListModel *g_monitors;  /* di proprieta' del display     */

/* ---- IPC Hyprland (stesso schema di workspaces.c) ----------------------- */

/* Path di un socket Hyprland nella dir dell'istanza corrente. */
static char *hypr_socket_path(const char *name)
{
    const char *sig = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig)
        return NULL;
    return g_build_filename(g_get_user_runtime_dir(), "hypr", sig, name, NULL);
}

/* Richiesta sincrona sul socket comandi: ritorna la risposta (g_free) o NULL. */
static char *hypr_request(const char *request)
{
    char *cmd_path = hypr_socket_path(".socket.sock");
    if (!cmd_path)
        return NULL;

    GSocketAddress *addr = g_unix_socket_address_new(cmd_path);
    GSocket *sock = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                 G_SOCKET_PROTOCOL_DEFAULT, NULL);
    char *result = NULL;

    if (sock && g_socket_connect(sock, addr, NULL, NULL) &&
        g_socket_send(sock, request, strlen(request), NULL, NULL) >= 0) {
        GString *s = g_string_new(NULL);
        char buf[4096];
        gssize n;
        while ((n = g_socket_receive(sock, buf, sizeof buf, NULL, NULL)) > 0)
            g_string_append_len(s, buf, (gsize) n);
        result = g_string_free(s, FALSE);
    }

    if (sock) {
        g_socket_close(sock, NULL);
        g_object_unref(sock);
    }
    g_object_unref(addr);
    g_free(cmd_path);
    return result;
}

/* ---- Layout dei monitor ------------------------------------------------- */

/* Layout dei monitor letto da Hyprland: connector -> MonRect* (logico).
 * Ritorna NULL se Hyprland non e' disponibile. Da distruggere dal chiamante. */
static GHashTable *hypr_monitor_layout(void)
{
    char *json = hypr_request("j/monitors");
    if (!json)
        return NULL;

    GHashTable *map = NULL;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, json, -1, NULL)) {
        JsonNode  *root = json_parser_get_root(parser);
        JsonArray *arr  = root ? json_node_get_array(root) : NULL;
        guint n = arr ? json_array_get_length(arr) : 0;

        map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        for (guint i = 0; i < n; i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            if (!o || !json_object_has_member(o, "name"))
                continue;
            if (json_object_has_member(o, "disabled") &&
                json_object_get_boolean_member(o, "disabled"))
                continue;

            const char *name = json_object_get_string_member(o, "name");
            double scale = json_object_has_member(o, "scale")
                         ? json_object_get_double_member(o, "scale") : 1.0;
            if (scale <= 0)
                scale = 1.0;
            int pw = json_object_has_member(o, "width")
                   ? (int) json_object_get_int_member(o, "width") : 0;
            int ph = json_object_has_member(o, "height")
                   ? (int) json_object_get_int_member(o, "height") : 0;
            int tr = json_object_has_member(o, "transform")
                   ? (int) json_object_get_int_member(o, "transform") : 0;
            int x = json_object_has_member(o, "x")
                  ? (int) json_object_get_int_member(o, "x") : 0;
            int y = json_object_has_member(o, "y")
                  ? (int) json_object_get_int_member(o, "y") : 0;

            /* Dimensione logica = pixel / scale. Le trasformazioni dispari
             * (90/270) scambiano gli assi. */
            int lw = (int) lround(pw / scale);
            int lh = (int) lround(ph / scale);
            if (tr % 2 != 0) {
                int t = lw; lw = lh; lh = t;
            }
            if (!name || lw <= 0 || lh <= 0)
                continue;

            MonRect *r = g_new0(MonRect, 1);
            r->x = x; r->y = y; r->w = lw; r->h = lh;
            g_hash_table_insert(map, g_strdup(name), r);
        }
    }
    g_object_unref(parser);
    g_free(json);

    if (map && g_hash_table_size(map) == 0) {
        g_hash_table_destroy(map);
        map = NULL;
    }
    return map;
}

/* Fallback compositor-agnostico: layout dalle geometrie GDK dei monitor. */
static GHashTable *gdk_monitor_layout(void)
{
    if (!g_monitors)
        return NULL;

    GHashTable *map =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    guint n = g_list_model_get_n_items(g_monitors);
    for (guint i = 0; i < n; i++) {
        GdkMonitor *mon = g_list_model_get_item(g_monitors, i);  /* +1 ref */
        const char *conn = gdk_monitor_get_connector(mon);
        if (conn && gdk_monitor_is_valid(mon)) {
            GdkRectangle geo;
            gdk_monitor_get_geometry(mon, &geo);
            MonRect *r = g_new0(MonRect, 1);
            r->x = geo.x; r->y = geo.y; r->w = geo.width; r->h = geo.height;
            g_hash_table_insert(map, g_strdup(conn), r);
        }
        g_object_unref(mon);
    }
    if (g_hash_table_size(map) == 0) {
        g_hash_table_destroy(map);
        return NULL;
    }
    return map;
}

/* Bounding box logico di tutti i monitor nel layout. FALSE se degenere. */
static gboolean layout_bbox(GHashTable *layout,
                            int *x0, int *y0, int *x1, int *y1)
{
    GHashTableIter it;
    gpointer k, v;
    gboolean any = FALSE;
    g_hash_table_iter_init(&it, layout);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        MonRect *r = v;
        if (!any) {
            *x0 = r->x; *y0 = r->y;
            *x1 = r->x + r->w; *y1 = r->y + r->h;
            any = TRUE;
        } else {
            if (r->x < *x0) *x0 = r->x;
            if (r->y < *y0) *y0 = r->y;
            if (r->x + r->w > *x1) *x1 = r->x + r->w;
            if (r->y + r->h > *y1) *y1 = r->y + r->h;
        }
    }
    return any && (*x1 > *x0) && (*y1 > *y0);
}

/* ---- Finestre di sfondo ------------------------------------------------- */

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

/* Imposta su un monitor il ritaglio del wallpaper esteso.
 * (bx0,by0) e (bboxW,bboxH) sono l'origine e le dimensioni del bounding box
 * logico di tutti i monitor; r e' il rettangolo logico di QUESTO monitor.
 * L'immagine viene scalata per COPRIRE l'intero bounding box (i bordi in
 * eccesso vengono ritagliati e centrati), poi ad ogni monitor tocca la sua
 * porzione: il risultato e' un'unica immagine continua su piu' schermi. */
static gboolean wp_set_extended(WpWin *w, GdkPixbuf *src, const MonRect *r,
                                int bx0, int by0, int bboxW, int bboxH)
{
    int imgW = gdk_pixbuf_get_width(src);
    int imgH = gdk_pixbuf_get_height(src);
    if (imgW <= 0 || imgH <= 0 || bboxW <= 0 || bboxH <= 0)
        return FALSE;

    /* COVER del bounding box: scala uniforme che riempie tutta l'area logica. */
    double scale = fmax((double) bboxW / imgW, (double) bboxH / imgH);
    double scaledW = imgW * scale;
    double scaledH = imgH * scale;
    /* Margini tagliati dal cover (l'immagine e' centrata sul bounding box). */
    double offX = (scaledW - bboxW) / 2.0;
    double offY = (scaledH - bboxH) / 2.0;

    /* Regione del monitor nello spazio immagine-scalata -> pixel sorgente. */
    double sx = (offX + (r->x - bx0)) / scale;
    double sy = (offY + (r->y - by0)) / scale;
    double sw = r->w / scale;
    double sh = r->h / scale;

    int ix = (int) floor(sx);
    int iy = (int) floor(sy);
    int iw = (int) ceil(sx + sw) - ix;
    int ih = (int) ceil(sy + sh) - iy;

    /* Clamp ai limiti dell'immagine. */
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix + iw > imgW) iw = imgW - ix;
    if (iy + ih > imgH) ih = imgH - iy;
    if (iw <= 0 || ih <= 0)
        return FALSE;

    /* Sub-pixbuf (condivide i pixel) poi copia compatta: il buffer parte da 0,
     * cosi' possiamo costruire una GdkMemoryTexture (API non deprecata). */
    GdkPixbuf *sub  = gdk_pixbuf_new_subpixbuf(src, ix, iy, iw, ih);
    GdkPixbuf *crop = gdk_pixbuf_copy(sub);
    g_object_unref(sub);
    if (!crop)
        return FALSE;

    GBytes *bytes = gdk_pixbuf_read_pixel_bytes(crop);
    GdkMemoryFormat fmt = gdk_pixbuf_get_has_alpha(crop)
                        ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
    GdkTexture *tex = gdk_memory_texture_new(
        gdk_pixbuf_get_width(crop), gdk_pixbuf_get_height(crop),
        fmt, bytes, gdk_pixbuf_get_rowstride(crop));

    gtk_picture_set_paintable(GTK_PICTURE(w->pic), GDK_PAINTABLE(tex));
    gtk_picture_set_content_fit(GTK_PICTURE(w->pic), GTK_CONTENT_FIT_COVER);

    g_object_unref(tex);
    g_bytes_unref(bytes);
    g_object_unref(crop);
    return TRUE;
}

/* Ricrea/aggiorna gli sfondi per l'insieme di monitor attuale. */
static void wp_apply(void)
{
    if (!g_monitors)
        return;

    /* Wallpaper esteso: carica una sola volta l'immagine e il layout. */
    const char *ext_path = config_get("wallpaper-extended");
    GdkPixbuf  *ext_src  = NULL;
    GHashTable *layout   = NULL;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    gboolean ext_ok = FALSE;
    if (ext_path && *ext_path) {
        ext_src = gdk_pixbuf_new_from_file(ext_path, NULL);
        layout  = hypr_monitor_layout();     /* posizioni da Hyprland      */
        if (!layout)
            layout = gdk_monitor_layout();    /* fallback: geometrie GDK    */
        if (ext_src && layout &&
            layout_bbox(layout, &bx0, &by0, &bx1, &by1))
            ext_ok = TRUE;
    }

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

            gboolean shown = FALSE;

            /* 1) Override per-monitor: immagine dedicata (cover). */
            char *key = g_strconcat("wallpaper-", conn, NULL);
            const char *specific = config_get(key);
            g_free(key);
            if (specific && *specific) {
                gtk_picture_set_filename(GTK_PICTURE(w->pic), specific);
                gtk_picture_set_content_fit(GTK_PICTURE(w->pic),
                                            GTK_CONTENT_FIT_COVER);
                shown = TRUE;
            }

            /* 2) Wallpaper esteso: ritaglio del panorama per questo monitor. */
            if (!shown && ext_ok) {
                MonRect *r = g_hash_table_lookup(layout, conn);
                if (r && wp_set_extended(w, ext_src, r,
                                         bx0, by0, bx1 - bx0, by1 - by0))
                    shown = TRUE;
            }

            /* 3) Wallpaper generico (cover). */
            if (!shown) {
                const char *generic = config_get("wallpaper");
                if (generic && *generic) {
                    gtk_picture_set_filename(GTK_PICTURE(w->pic), generic);
                    gtk_picture_set_content_fit(GTK_PICTURE(w->pic),
                                                GTK_CONTENT_FIT_COVER);
                    shown = TRUE;
                }
            }

            gtk_widget_set_visible(w->win, shown);
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
    if (layout)
        g_hash_table_destroy(layout);
    if (ext_src)
        g_object_unref(ext_src);
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
