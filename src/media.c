#include "media.h"
#include <gio/gio.h>
#include <string.h>

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
static GtkWidget       *m_box;         /* intero modulo (nascosto se vuoto) */
static GtkWidget       *m_title;       /* label titolo                      */
static GtkWidget       *m_pp_icon;     /* label glyph play/pausa            */
static char            *m_player;      /* bus name del player corrente      */
static guint            m_props_sub;   /* sub PropertiesChanged del player  */

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

static void on_info_ready(GObject *src, GAsyncResult *res,
                          gpointer u G_GNUC_UNUSED)
{
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  res, NULL);
    if (!ret) {
        gtk_widget_set_visible(m_box, FALSE);
        return;
    }

    GVariant *props = g_variant_get_child_value(ret, 0);

    const char *status = NULL;
    g_variant_lookup(props, "PlaybackStatus", "&s", &status);
    gtk_label_set_text(GTK_LABEL(m_pp_icon),
                       (status && g_strcmp0(status, "Playing") == 0)
                       ? "pause" : "play_arrow");

    const char *title = NULL;
    GVariant *meta = g_variant_lookup_value(props, "Metadata",
                                            G_VARIANT_TYPE("a{sv}"));
    if (meta)
        g_variant_lookup(meta, "xesam:title", "&s", &title);

    gtk_label_set_text(GTK_LABEL(m_title),
                       (title && *title) ? title : "");
    gtk_widget_set_visible(m_box, TRUE);

    if (meta)
        g_variant_unref(meta);
    g_variant_unref(props);
    g_variant_unref(ret);
}

static void update_info(void)
{
    if (!m_player) {
        gtk_widget_set_visible(m_box, FALSE);
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

/* ---- UI ----------------------------------------------------------------- */

static GtkWidget *icon_button(const char *ligature, GCallback cb)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "media-btn");
    gtk_widget_set_focusable(btn, FALSE);
    GtkWidget *icon = gtk_label_new(ligature);
    gtk_widget_add_css_class(icon, "media-icon");
    gtk_button_set_child(GTK_BUTTON(btn), icon);
    g_signal_connect(btn, "clicked", cb, NULL);
    return btn;
}

GtkWidget *media_new(void)
{
    m_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

    m_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(m_box, "media");
    gtk_widget_set_valign(m_box, GTK_ALIGN_CENTER);

    m_title = gtk_label_new("");
    gtk_widget_add_css_class(m_title, "media-title");
    gtk_label_set_ellipsize(GTK_LABEL(m_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(m_title), 28);
    gtk_box_append(GTK_BOX(m_box), m_title);

    gtk_box_append(GTK_BOX(m_box),
                   icon_button("skip_previous", G_CALLBACK(on_prev)));

    GtkWidget *pp = gtk_button_new();
    gtk_widget_add_css_class(pp, "media-btn");
    gtk_widget_set_focusable(pp, FALSE);
    m_pp_icon = gtk_label_new("play_arrow");
    gtk_widget_add_css_class(m_pp_icon, "media-icon");
    gtk_button_set_child(GTK_BUTTON(pp), m_pp_icon);
    g_signal_connect(pp, "clicked", G_CALLBACK(on_playpause), NULL);
    gtk_box_append(GTK_BOX(m_box), pp);

    gtk_box_append(GTK_BOX(m_box),
                   icon_button("skip_next", G_CALLBACK(on_next)));

    if (m_bus) {
        g_dbus_connection_signal_subscribe(
            m_bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
            "NameOwnerChanged", "/org/freedesktop/DBus", NULL,
            G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, NULL, NULL);
        choose_player();
    }

    gtk_widget_set_visible(m_box, FALSE);        /* finche' non c'e' un player */
    return m_box;
}
