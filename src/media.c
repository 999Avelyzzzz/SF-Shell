#include "media.h"
#include <gio/gio.h>
#include <string.h>
#include <math.h>

/* ========================================================================
 *  Media player via MPRIS (org.mpris.MediaPlayer2.*):
 *  - titolo del brano corrente
 *  - tasti indietro / play-pausa / avanti (Material Symbols)
 *  Sceglie un player attivo, preferendo quello in Playing, e segue i
 *  cambiamenti (PropertiesChanged) e l'arrivo/uscita dei player.
 * ======================================================================== */

#define MPRIS_PREFIX  "org.mpris.MediaPlayer2."
#define MPRIS_PATH    "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER  "org.mpris.MediaPlayer2.Player"

static GDBusConnection *m_bus;
static char            *m_player;      /* bus name del player corrente      */
static guint            m_props_sub;   /* sub PropertiesChanged del player  */

/* Una copia del modulo media per ogni barra/monitor. Il player e lo stato
 * (titolo, stato play) sono globali e condivisi: qui teniamo solo i widget da
 * aggiornare in parallelo. */
typedef struct {
    GtkWidget *box;        /* intero modulo (nascosto se vuoto) */
    GtkWidget *title;      /* area disegno titolo (marquee)     */
    GtkWidget *pp_icon;    /* label glyph play/pausa            */
    guint      tick;       /* tick callback marquee (0 = fermo) */
} MediaView;

static GPtrArray *m_views;             /* MediaView*                        */

/* ---- Marquee del titolo -------------------------------------------------- *
 * Se il titolo supera SCROLL_MAX_CHARS caratteri il testo scorre in loop
 * continuo (super smooth, basato sul frame clock) con dissolvenza ai bordi.
 * La larghezza visibile e' bloccata a ~30 caratteri; ogni copia del testo e'
 * separata da SCROLL_GAP px, cosi' lo scorrimento appare infinito.          */
#define SCROLL_MAX_CHARS 30      /* soglia oltre la quale parte lo scroll     */
#define SCROLL_SPEED     45.0    /* px al secondo                             */
#define SCROLL_GAP       48      /* stacco (px) tra una ripetizione e l'altra */
#define SCROLL_FADE      30      /* larghezza (px) della dissolvenza ai bordi */

static char    *m_title_text;          /* testo corrente del brano          */
static gboolean m_scroll;              /* true se il titolo va fatto scorrere*/
static int      m_text_w;              /* larghezza (px) del testo intero   */
static int      m_text_h;              /* altezza (px) del testo            */
static int      m_view_w;              /* larghezza (px) della finestra visibile */
static double   m_offset;              /* scostamento corrente (px, condiviso)*/
static gint64   m_anim_start;          /* frame time d'inizio animazione (us)*/

static void media_set_title(const char *text);

/* ---- Comandi ------------------------------------------------------------ */

static void player_call(const char *method)
{
    if (!m_player)
        return;
    g_dbus_connection_call(m_bus, m_player, MPRIS_PATH, MPRIS_PLAYER, method,
                           NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                           NULL, NULL, NULL);
}

static void on_prev(GtkButton *b G_GNUC_UNUSED, gpointer u G_GNUC_UNUSED)
{ player_call("Previous"); }
static void on_playpause(GtkButton *b G_GNUC_UNUSED, gpointer u G_GNUC_UNUSED)
{ player_call("PlayPause"); }
static void on_next(GtkButton *b G_GNUC_UNUSED, gpointer u G_GNUC_UNUSED)
{ player_call("Next"); }

/* ---- Aggiornamento info ------------------------------------------------- */

/* Mostra/nasconde il modulo su tutte le barre. */
static void media_set_visible(gboolean visible)
{
    if (!m_views)
        return;
    for (guint i = 0; i < m_views->len; i++) {
        MediaView *v = m_views->pdata[i];
        gtk_widget_set_visible(v->box, visible);
    }
}

/* Aggiorna il glyph play/pausa su tutte le barre. */
static void media_set_playpause(gboolean playing)
{
    if (!m_views)
        return;
    for (guint i = 0; i < m_views->len; i++) {
        MediaView *v = m_views->pdata[i];
        gtk_label_set_text(GTK_LABEL(v->pp_icon),
                           playing ? "pause" : "play_arrow");
    }
}

static void on_info_ready(GObject *src, GAsyncResult *res,
                          gpointer u G_GNUC_UNUSED)
{
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  res, NULL);
    if (!ret) {
        media_set_visible(FALSE);
        return;
    }

    GVariant *props = g_variant_get_child_value(ret, 0);

    const char *status = NULL;
    g_variant_lookup(props, "PlaybackStatus", "&s", &status);
    media_set_playpause(status && g_strcmp0(status, "Playing") == 0);

    const char *title = NULL;
    GVariant *meta = g_variant_lookup_value(props, "Metadata",
                                            G_VARIANT_TYPE("a{sv}"));
    if (meta)
        g_variant_lookup(meta, "xesam:title", "&s", &title);

    media_set_title((title && *title) ? title : "");
    media_set_visible(TRUE);

    if (meta)
        g_variant_unref(meta);
    g_variant_unref(props);
    g_variant_unref(ret);
}

static void update_info(void)
{
    if (!m_player) {
        media_set_visible(FALSE);
        return;
    }
    g_dbus_connection_call(m_bus, m_player, MPRIS_PATH,
                           "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", MPRIS_PLAYER),
                           G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           on_info_ready, NULL);
}

static void on_props_changed(GDBusConnection *c G_GNUC_UNUSED,
                             const char *s G_GNUC_UNUSED,
                             const char *p G_GNUC_UNUSED,
                             const char *i G_GNUC_UNUSED,
                             const char *sig G_GNUC_UNUSED,
                             GVariant *params G_GNUC_UNUSED,
                             gpointer u G_GNUC_UNUSED)
{
    update_info();
}

/* ---- Scelta del player -------------------------------------------------- */

static void set_current(const char *name)
{
    if (g_strcmp0(name, m_player) == 0)
        return;                                   /* invariato */

    if (m_props_sub) {
        g_dbus_connection_signal_unsubscribe(m_bus, m_props_sub);
        m_props_sub = 0;
    }
    g_free(m_player);
    m_player = g_strdup(name);

    if (m_player)
        m_props_sub = g_dbus_connection_signal_subscribe(
            m_bus, m_player, "org.freedesktop.DBus.Properties",
            "PropertiesChanged", MPRIS_PATH, NULL,
            G_DBUS_SIGNAL_FLAGS_NONE, on_props_changed, NULL, NULL);

    update_info();
}

/* PlaybackStatus di un player (sync, veloce). "" se non leggibile. */
static char *player_status(const char *name)
{
    GVariant *ret = g_dbus_connection_call_sync(
        m_bus, name, MPRIS_PATH, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", MPRIS_PLAYER, "PlaybackStatus"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (!ret)
        return g_strdup("");
    GVariant *box;
    g_variant_get(ret, "(v)", &box);
    char *s = g_variant_dup_string(box, NULL);
    g_variant_unref(box);
    g_variant_unref(ret);
    return s;
}

static void on_names_ready(GObject *src, GAsyncResult *res,
                           gpointer u G_GNUC_UNUSED)
{
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  res, NULL);
    if (!ret)
        return;

    GVariant *arr;
    g_variant_get(ret, "(@as)", &arr);

    char *first = NULL, *playing = NULL;
    GVariantIter it;
    const char *name;
    g_variant_iter_init(&it, arr);
    while (g_variant_iter_next(&it, "&s", &name)) {
        if (!g_str_has_prefix(name, MPRIS_PREFIX))
            continue;
        if (!first)
            first = g_strdup(name);
        if (!playing) {
            char *st = player_status(name);
            if (g_strcmp0(st, "Playing") == 0)
                playing = g_strdup(name);
            g_free(st);
        }
    }

    set_current(playing ? playing : first);      /* NULL => nasconde */

    g_free(first);
    g_free(playing);
    g_variant_unref(arr);
    g_variant_unref(ret);
}

static void choose_player(void)
{
    g_dbus_connection_call(m_bus, "org.freedesktop.DBus",
                           "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           "ListNames", NULL, G_VARIANT_TYPE("(as)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           on_names_ready, NULL);
}

static void on_name_owner_changed(GDBusConnection *c G_GNUC_UNUSED,
                                  const char *s G_GNUC_UNUSED,
                                  const char *p G_GNUC_UNUSED,
                                  const char *i G_GNUC_UNUSED,
                                  const char *sig G_GNUC_UNUSED,
                                  GVariant *params, gpointer u G_GNUC_UNUSED)
{
    const char *name, *old_owner, *new_owner;
    g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
    if (!g_str_has_prefix(name, MPRIS_PREFIX))
        return;
    /* Un player e' sparito o comparso: rivaluta la scelta. */
    if (*new_owner == '\0' && g_strcmp0(name, m_player) == 0)
        set_current(NULL);
    choose_player();
}

/* ---- Marquee: disegno e animazione ------------------------------------- */

static gboolean on_title_tick(GtkWidget *w, GdkFrameClock *clock,
                              gpointer u G_GNUC_UNUSED)
{
    gint64 t = gdk_frame_clock_get_frame_time(clock);   /* microsecondi */
    if (m_anim_start == 0)
        m_anim_start = t;
    double elapsed = (t - m_anim_start) / 1e6;
    double period = m_text_w + SCROLL_GAP;
    if (period > 0.0)
        m_offset = fmod(elapsed * SCROLL_SPEED, period);
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static void media_draw(GtkDrawingArea *area, cairo_t *cr,
                       int width, int height, gpointer u G_GNUC_UNUSED)
{
    if (!m_title_text || !*m_title_text)
        return;

    GtkWidget *w = GTK_WIDGET(area);
    GdkRGBA color;
    gtk_widget_get_color(w, &color);

    PangoLayout *layout = gtk_widget_create_pango_layout(w, m_title_text);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    double y = (height - th) / 2.0;

    if (!m_scroll) {
        gdk_cairo_set_source_rgba(cr, &color);
        cairo_move_to(cr, 0, y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        return;
    }

    /* Testo ripetuto su un gruppo separato, poi mascherato ai bordi. */
    double period = m_text_w + SCROLL_GAP;
    cairo_push_group(cr);
    gdk_cairo_set_source_rgba(cr, &color);
    for (double x = -m_offset; x < width; x += period) {
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
        if (period <= 0.0)
            break;
    }
    cairo_pop_group_to_source(cr);

    /* Dissolvenza (alpha) SCROLL_FADE px a sinistra e a destra. */
    double f = width > 0 ? (double) SCROLL_FADE / (double) width : 0.0;
    if (f > 0.49)
        f = 0.49;
    cairo_pattern_t *mask = cairo_pattern_create_linear(0, 0, width, 0);
    cairo_pattern_add_color_stop_rgba(mask, 0.0,       0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(mask, f,         0, 0, 0, 1);
    cairo_pattern_add_color_stop_rgba(mask, 1.0 - f,   0, 0, 0, 1);
    cairo_pattern_add_color_stop_rgba(mask, 1.0,       0, 0, 0, 0);
    cairo_mask(cr, mask);
    cairo_pattern_destroy(mask);

    g_object_unref(layout);
}

/* Applica lo stato marquee corrente (globale) a un singolo view: dimensioni
 * dell'area, avvio/arresto del tick, ridisegno. */
static void media_view_apply(MediaView *v)
{
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->title), m_view_w);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->title), m_text_h);

    if (m_scroll && !v->tick)
        v->tick = gtk_widget_add_tick_callback(v->title, on_title_tick,
                                               NULL, NULL);
    else if (!m_scroll && v->tick) {
        gtk_widget_remove_tick_callback(v->title, v->tick);
        v->tick = 0;
    }
    gtk_widget_queue_draw(v->title);
}

static void media_set_title(const char *text)
{
    const char *safe = (text && *text) ? text : "";
    if (g_strcmp0(safe, m_title_text) == 0)
        return;                                   /* invariato: niente reset */

    g_free(m_title_text);
    m_title_text = g_strdup(safe);

    if (!m_views || m_views->len == 0)
        return;

    /* Misure col font condiviso (stessa classe CSS su ogni barra): basta un
     * qualsiasi view come contesto pango. */
    GtkWidget *probe = ((MediaView *) m_views->pdata[0])->title;
    PangoLayout *layout = gtk_widget_create_pango_layout(probe, m_title_text);
    pango_layout_get_pixel_size(layout, &m_text_w, &m_text_h);

    long nchars = g_utf8_strlen(m_title_text, -1);
    m_scroll = nchars > SCROLL_MAX_CHARS;

    if (m_scroll) {
        /* Finestra visibile = larghezza dei primi SCROLL_MAX_CHARS caratteri. */
        const char *end = g_utf8_offset_to_pointer(m_title_text, SCROLL_MAX_CHARS);
        char *head = g_strndup(m_title_text, end - m_title_text);
        PangoLayout *hl = gtk_widget_create_pango_layout(probe, head);
        int hw;
        pango_layout_get_pixel_size(hl, &hw, NULL);
        m_view_w = hw;
        g_free(head);
        g_object_unref(hl);
    } else {
        m_view_w = m_text_w;
    }
    g_object_unref(layout);

    m_offset = 0;
    m_anim_start = 0;

    for (guint i = 0; i < m_views->len; i++)
        media_view_apply(m_views->pdata[i]);
}

/* ---- UI ----------------------------------------------------------------- */

static GtkWidget *icon_button(const char *ligature, GCallback cb)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "media-btn");
    gtk_widget_set_focusable(btn, FALSE);
    GtkWidget *icon = gtk_label_new(ligature);
    gtk_widget_add_css_class(icon, "media-icon");
    gtk_widget_add_css_class(icon, "media-skip");   /* prev/next: glifo piu' grande */
    gtk_button_set_child(GTK_BUTTON(btn), icon);
    g_signal_connect(btn, "clicked", cb, NULL);
    return btn;
}

/* Un view distrutto (barra chiusa): toglilo dalla lista. */
static void media_view_free(gpointer data, GObject *where G_GNUC_UNUSED)
{
    if (m_views)
        g_ptr_array_remove_fast(m_views, data);
    g_free(data);
}

GtkWidget *media_new(void)
{
    if (!m_views)
        m_views = g_ptr_array_new();

    MediaView *v = g_new0(MediaView, 1);

    v->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(v->box, "media");
    gtk_widget_set_valign(v->box, GTK_ALIGN_CENTER);

    v->title = gtk_drawing_area_new();
    gtk_widget_add_css_class(v->title, "media-title");
    gtk_widget_set_valign(v->title, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->title),
                                   media_draw, NULL, NULL);
    gtk_box_append(GTK_BOX(v->box), v->title);

    gtk_box_append(GTK_BOX(v->box),
                   icon_button("skip_previous", G_CALLBACK(on_prev)));

    GtkWidget *pp = gtk_button_new();
    gtk_widget_add_css_class(pp, "media-btn");
    gtk_widget_set_focusable(pp, FALSE);
    v->pp_icon = gtk_label_new("play_arrow");
    gtk_widget_add_css_class(v->pp_icon, "media-icon");
    gtk_button_set_child(GTK_BUTTON(pp), v->pp_icon);
    g_signal_connect(pp, "clicked", G_CALLBACK(on_playpause), NULL);
    gtk_box_append(GTK_BOX(v->box), pp);

    gtk_box_append(GTK_BOX(v->box),
                   icon_button("skip_next", G_CALLBACK(on_next)));

    g_ptr_array_add(m_views, v);
    g_object_weak_ref(G_OBJECT(v->box), media_view_free, v);

    /* Il bus e la scelta del player vanno inizializzati una sola volta. */
    if (!m_bus) {
        m_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        if (m_bus) {
            g_dbus_connection_signal_subscribe(
                m_bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
                "NameOwnerChanged", "/org/freedesktop/DBus", NULL,
                G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, NULL, NULL);
            choose_player();
        }
    } else {
        /* Barra aggiunta a caldo: allinea subito il nuovo view allo stato. */
        if (m_title_text)
            media_view_apply(v);
        update_info();
    }

    gtk_widget_set_visible(v->box, FALSE);        /* finche' non c'e' un player */
    return v->box;
}
