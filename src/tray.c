#include "tray.h"
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

/* ========================================================================
 *  System tray: StatusNotifierWatcher + Host (protocollo KDE SNI) e menu
 *  contestuale costruito dalle entries di com.canonical.dbusmenu.
 *
 *  Su Hyprland di norma non esiste un watcher: lo implementiamo noi. Se un
 *  watcher esiste gia' (altra shell), entriamo in modalita' client.
 * ======================================================================== */

#define SNI_WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_PATH  "/StatusNotifierWatcher"
#define SNI_WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define SNI_ITEM_IFACE    "org.kde.StatusNotifierItem"
#define DBUSMENU_IFACE    "com.canonical.dbusmenu"
#define ICON_SIZE 18

static GDBusConnection *g_bus;         /* session bus                       */
static GtkWidget       *g_icons_box;   /* contenitore delle icone           */
static GHashTable      *g_items;       /* key "bus\npath" -> Item*          */
static GHashTable      *g_registered;  /* set di service string (watcher)   */
static GDBusNodeInfo   *g_watcher_info;
static guint            g_watcher_obj_id;
static gboolean         g_is_watcher;
static char            *g_host_name;

static GtkWidget       *g_revealer;    /* revealer delle icone              */
static GtkWidget       *g_toggle;      /* bottone freccia                   */
static guint            g_close_timer; /* timeout auto-chiusura             */
static int              g_menu_open;   /* numero di menu contestuali aperti */
static gboolean         g_pointer_inside; /* puntatore sopra il tray         */

typedef struct {
    char      *bus;        /* nome del bus del servizio                     */
    char      *path;       /* object path dell'item                         */
    char      *key;        /* "bus\npath"                                   */
    char      *menu_path;  /* object path del dbusmenu (o NULL)             */
    GtkWidget *button;
    GtkWidget *image;
    guint      name_watch;
    guint      signal_sub;
    guint      fetch_tries; /* retry per app lente a esportare l'oggetto     */
} Item;

static void add_item(const char *bus, const char *path);
static void remove_item(const char *key);

/* ---- Auto-chiusura del tray -------------------------------------------- */

#define TRAY_CLOSE_MS 2000

static void tray_collapse(void)
{
    if (g_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(g_revealer), FALSE);
    if (g_toggle)
        gtk_widget_remove_css_class(g_toggle, "expanded");
}

static gboolean close_timer_cb(gpointer data G_GNUC_UNUSED)
{
    g_close_timer = 0;
    if (g_menu_open <= 0)
        tray_collapse();
    return G_SOURCE_REMOVE;
}

static void tray_cancel_close(void)
{
    if (g_close_timer) {
        g_source_remove(g_close_timer);
        g_close_timer = 0;
    }
}

/* (Ri)avvia il conto alla rovescia: solo se il tray e' aperto, nessun menu
 * contestuale e' attivo e il puntatore NON e' sopra il tray. Finche' il
 * mouse resta sulla freccia o su un'icona il tray non si ricompatta, anche
 * se il puntatore e' fermo (nessun evento di movimento). */
static void tray_schedule_close(void)
{
    tray_cancel_close();
    if (g_pointer_inside)
        return;
    if (g_menu_open <= 0 && g_revealer
        && gtk_revealer_get_reveal_child(GTK_REVEALER(g_revealer)))
        g_close_timer = g_timeout_add(TRAY_CLOSE_MS, close_timer_cb, NULL);
}

/* Menu contestuale aperto: sospende l'auto-chiusura. */
static void tray_menu_opened(void)
{
    g_menu_open++;
    tray_cancel_close();
}

/* Menu contestuale chiuso: quando sono tutti chiusi, riparte il conto. */
static void tray_menu_closed(void)
{
    if (--g_menu_open <= 0) {
        g_menu_open = 0;
        tray_schedule_close();
    }
}

/* ---- Utilita' ----------------------------------------------------------- */

/* L'argomento di RegisterStatusNotifierItem puo' essere un bus name, un
 * object path (allora il bus e' il mittente), oppure "bus/path". */
static void parse_service(const char *service, const char *sender,
                          char **out_bus, char **out_path)
{
    if (service && service[0] == '/') {
        *out_bus  = g_strdup(sender ? sender : "");
        *out_path = g_strdup(service);
    } else if (service && strchr(service, '/')) {
        const char *slash = strchr(service, '/');
        *out_bus  = g_strndup(service, slash - service);
        *out_path = g_strdup(slash);
    } else {
        *out_bus  = g_strdup(service ? service : "");
        *out_path = g_strdup("/StatusNotifierItem");
    }
}

static char *make_key(const char *bus, const char *path)
{
    return g_strdup_printf("%s\n%s", bus, path);
}

/* ---- Icone -------------------------------------------------------------- */

/* Costruisce una GdkTexture dal piu' grande pixmap ARGB32 (network order). */
static GdkTexture *texture_from_pixmap(GVariant *arr)
{
    GVariantIter iter;
    gint32 w, h, best_w = 0, best_h = 0;
    GVariant *bytes, *best = NULL;

    g_variant_iter_init(&iter, arr);
    while (g_variant_iter_next(&iter, "(ii@ay)", &w, &h, &bytes)) {
        if (w > 0 && h > 0 && (gint64) w * h > (gint64) best_w * best_h) {
            if (best)
                g_variant_unref(best);
            best = g_variant_ref(bytes);
            best_w = w;
            best_h = h;
        }
        g_variant_unref(bytes);
    }
    if (!best)
        return NULL;

    gsize n = 0;
    const guchar *data = g_variant_get_fixed_array(best, &n, 1);
    GdkTexture *tex = NULL;
    if (data && n >= (gsize) best_w * best_h * 4) {
        GBytes *gb = g_bytes_new(data, (gsize) best_w * best_h * 4);
        tex = gdk_memory_texture_new(best_w, best_h,
                                     GDK_MEMORY_A8R8G8B8, gb, best_w * 4);
        g_bytes_unref(gb);
    }
    g_variant_unref(best);
    return tex;
}

static void fetch_props(Item *it);

/* Riprova la lettura proprietà (chiave passata come stringa, safe se l'item
 * e' stato rimosso nel frattempo). */
static gboolean refetch_cb(gpointer data)
{
    Item *it = g_hash_table_lookup(g_items, (const char *) data);
    if (it)
        fetch_props(it);
    return G_SOURCE_REMOVE;
}

static void on_props_ready(GObject *src, GAsyncResult *res, gpointer user_data)
{
    Item *it = user_data;
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  res, &err);
    if (!ret) {
        g_clear_error(&err);
        /* App ancora non pronta a rispondere (registra l'item prima di
         * esportare l'oggetto): riprova qualche volta. La rimozione vera
         * avviene per name-vanish o Status "Passive", mai qui: cosi' le app
         * gia' aperte all'avvio della shell non spariscono per una race. */
        if (g_hash_table_contains(g_items, it->key) && it->fetch_tries < 4) {
            it->fetch_tries++;
            g_timeout_add_full(G_PRIORITY_DEFAULT, 600, refetch_cb,
                               g_strdup(it->key), g_free);
        }
        return;
    }
    /* Se l'item e' stato rimosso nel frattempo. */
    if (!g_hash_table_contains(g_items, it->key)) {
        g_variant_unref(ret);
        return;
    }
    it->fetch_tries = 0;

    GVariant *props = g_variant_get_child_value(ret, 0);

    const char *icon_name = NULL, *theme_path = NULL, *title = NULL, *status = NULL;
    g_variant_lookup(props, "IconThemePath", "&s", &theme_path);
    g_variant_lookup(props, "IconName", "&s", &icon_name);
    g_variant_lookup(props, "Title", "&s", &title);
    g_variant_lookup(props, "Status", "&s", &status);

    /* Status "Passive" => l'app vuole nascondere l'icona. */
    gtk_widget_set_visible(it->button,
                           !(status && g_strcmp0(status, "Passive") == 0));

    const char *menu = NULL;
    if (g_variant_lookup(props, "Menu", "&o", &menu) && menu && *menu) {
        g_free(it->menu_path);
        it->menu_path = g_strdup(menu);
    }

    if (theme_path && *theme_path) {
        GtkIconTheme *theme =
            gtk_icon_theme_get_for_display(gdk_display_get_default());
        gtk_icon_theme_add_search_path(theme, theme_path);
    }

    gboolean set = FALSE;
    if (icon_name && *icon_name) {
        gtk_image_set_from_icon_name(GTK_IMAGE(it->image), icon_name);
        set = TRUE;
    }
    if (!set) {
        GVariant *pix = g_variant_lookup_value(props, "IconPixmap",
                                               G_VARIANT_TYPE("a(iiay)"));
        if (pix) {
            GdkTexture *tex = texture_from_pixmap(pix);
            if (tex) {
                gtk_image_set_from_paintable(GTK_IMAGE(it->image),
                                             GDK_PAINTABLE(tex));
                g_object_unref(tex);
                set = TRUE;
            }
            g_variant_unref(pix);
        }
    }
    if (!set)
        gtk_image_set_from_icon_name(GTK_IMAGE(it->image),
                                     "image-missing-symbolic");

    if (title && *title)
        gtk_widget_set_tooltip_text(it->button, title);

    g_variant_unref(props);
    g_variant_unref(ret);
}

static void fetch_props(Item *it)
{
    g_dbus_connection_call(g_bus, it->bus, it->path,
                           "org.freedesktop.DBus.Properties", "GetAll",
                           g_variant_new("(s)", SNI_ITEM_IFACE),
                           G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           on_props_ready, it);
}

/* ---- DBusMenu ----------------------------------------------------------- */

static void on_popover_closed(GtkPopover *pop, gpointer ud G_GNUC_UNUSED);

/* Una riga del menu: item + id dbusmenu + popover radice da chiudere. */
typedef struct {
    Item      *it;
    gint32     id;
    GtkWidget *root;
    GVariant  *children;   /* solo per i submenu (ref) */
} MenuRow;

static void menu_row_free(gpointer data, GClosure *closure G_GNUC_UNUSED)
{
    MenuRow *r = data;
    if (r->children)
        g_variant_unref(r->children);
    g_free(r);
}

static GtkWidget *menu_build(Item *it, GVariant *children, GtkWidget *root);

/* Click su una voce: manda Event(id,"clicked") e chiude il menu. */
static void on_row_activate(GtkButton *btn G_GNUC_UNUSED, gpointer data)
{
    MenuRow *r = data;
    if (!r->it->menu_path)
        return;
    g_dbus_connection_call(
        g_bus, r->it->bus, r->it->menu_path, DBUSMENU_IFACE, "Event",
        g_variant_new("(isvu)", r->id, "clicked",
                      g_variant_new_string(""), (guint32) 0),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    if (r->root)
        gtk_popover_popdown(GTK_POPOVER(r->root));
}

/* Click su un submenu: apre un popover figlio a lato. */
static void on_submenu_activate(GtkButton *btn, gpointer data)
{
    MenuRow *r = data;
    GtkWidget *sub = menu_build(r->it, r->children, r->root);
    gtk_widget_set_parent(sub, GTK_WIDGET(btn));
    gtk_popover_set_position(GTK_POPOVER(sub), GTK_POS_RIGHT);
    g_signal_connect(sub, "closed", G_CALLBACK(on_popover_closed), NULL);
    tray_menu_opened();
    gtk_popover_popup(GTK_POPOVER(sub));
}

/* Costruisce un GtkPopover con una riga GtkButton per ogni voce dbusmenu.
 * Click diretti (niente action muxer) -> attivazione garantita. */
static GtkWidget *menu_build(Item *it, GVariant *children, GtkWidget *root)
{
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_add_css_class(pop, "tray-menu");
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    if (!root)
        root = pop;

    GVariantIter iter;
    GVariant *node;
    g_variant_iter_init(&iter, children);
    while (g_variant_iter_next(&iter, "v", &node)) {
        gint32 id;
        GVariant *props, *kids;
        g_variant_get(node, "(i@a{sv}@av)", &id, &props, &kids);

        gboolean visible = TRUE, enabled = TRUE;
        g_variant_lookup(props, "visible", "b", &visible);
        g_variant_lookup(props, "enabled", "b", &enabled);
        const char *type = NULL, *label = NULL, *cdisp = NULL;
        g_variant_lookup(props, "type", "&s", &type);
        g_variant_lookup(props, "label", "&s", &label);
        g_variant_lookup(props, "children-display", "&s", &cdisp);

        if (!visible) {
            g_variant_unref(props); g_variant_unref(kids); g_variant_unref(node);
            continue;
        }
        if (type && g_strcmp0(type, "separator") == 0) {
            GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_add_css_class(sep, "tray-menu-sep");
            gtk_box_append(GTK_BOX(box), sep);
            g_variant_unref(props); g_variant_unref(kids); g_variant_unref(node);
            continue;
        }

        GtkWidget *lbl = gtk_label_new(label ? label : "");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_use_underline(GTK_LABEL(lbl), TRUE);
        GtkWidget *btn = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(btn), lbl);
        gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
        gtk_widget_add_css_class(btn, "tray-menu-item");
        gtk_widget_set_focusable(btn, FALSE);
        gtk_widget_set_sensitive(btn, enabled);

        MenuRow *r = g_new0(MenuRow, 1);
        r->it = it;
        r->id = id;
        r->root = root;

        if (cdisp && g_strcmp0(cdisp, "submenu") == 0
            && g_variant_n_children(kids) > 0) {
            r->children = g_variant_ref(kids);
            g_signal_connect_data(btn, "clicked",
                                  G_CALLBACK(on_submenu_activate), r,
                                  menu_row_free, 0);
        } else {
            g_signal_connect_data(btn, "clicked",
                                  G_CALLBACK(on_row_activate), r,
                                  menu_row_free, 0);
        }
        gtk_box_append(GTK_BOX(box), btn);

        g_variant_unref(props);
        g_variant_unref(kids);
        g_variant_unref(node);
    }
    return pop;
}

static void on_popover_closed(GtkPopover *pop, gpointer ud G_GNUC_UNUSED)
{
    tray_menu_closed();
    gtk_widget_unparent(GTK_WIDGET(pop));
}

static void on_layout_ready(GObject *src, GAsyncResult *res, gpointer user_data)
{
    Item *it = user_data;
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  res, &err);
    if (!ret) {
        g_clear_error(&err);
        return;
    }
    if (!g_hash_table_contains(g_items, it->key)) {
        g_variant_unref(ret);
        return;
    }

    guint revision;
    GVariant *root;
    g_variant_get(ret, "(u@(ia{sv}av))", &revision, &root);
    GVariant *children = g_variant_get_child_value(root, 2);

    GtkWidget *pop = menu_build(it, children, NULL);
    gtk_widget_set_parent(pop, it->button);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    g_signal_connect(pop, "closed", G_CALLBACK(on_popover_closed), NULL);
    tray_menu_opened();
    gtk_popover_popup(GTK_POPOVER(pop));

    g_variant_unref(children);
    g_variant_unref(root);
    g_variant_unref(ret);
}

static void show_menu(Item *it)
{
    if (!it->menu_path) {
        /* Nessun dbusmenu: chiediamo all'item di aprire il suo menu. */
        g_dbus_connection_call(g_bus, it->bus, it->path, SNI_ITEM_IFACE,
                               "ContextMenu", g_variant_new("(ii)", 0, 0),
                               NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                               NULL, NULL);
        return;
    }
    const char *props[] = { NULL };
    /* AboutToShow poi GetLayout, cosi' l'app aggiorna le voci. */
    g_dbus_connection_call(g_bus, it->bus, it->menu_path, DBUSMENU_IFACE,
                           "AboutToShow", g_variant_new("(i)", 0),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_dbus_connection_call(
        g_bus, it->bus, it->menu_path, DBUSMENU_IFACE, "GetLayout",
        g_variant_new("(ii^as)", 0, -1, props),
        G_VARIANT_TYPE("(u(ia{sv}av))"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_layout_ready, it);
}

/* ---- Interazione icona -------------------------------------------------- */

static void on_item_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    Item *it = user_data;
    g_dbus_connection_call(g_bus, it->bus, it->path, SNI_ITEM_IFACE,
                           "Activate", g_variant_new("(ii)", 0, 0),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void on_item_right_click(GtkGestureClick *g G_GNUC_UNUSED,
                                int n_press G_GNUC_UNUSED,
                                double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                                gpointer user_data)
{
    show_menu(user_data);
}

/* ---- Segnali SNI e ciclo di vita item ----------------------------------- */

static void on_item_signal(GDBusConnection *c G_GNUC_UNUSED,
                           const char *sender G_GNUC_UNUSED,
                           const char *path G_GNUC_UNUSED,
                           const char *iface G_GNUC_UNUSED,
                           const char *signal, GVariant *params G_GNUC_UNUSED,
                           gpointer user_data)
{
    if (g_str_has_prefix(signal, "New"))   /* NewIcon/NewStatus/NewTitle... */
        fetch_props(user_data);
}

static void on_name_vanished(GDBusConnection *c G_GNUC_UNUSED,
                             const char *name G_GNUC_UNUSED, gpointer user_data)
{
    Item *it = user_data;
    remove_item(it->key);
}

static void add_item(const char *bus, const char *path)
{
    char *key = make_key(bus, path);
    if (g_hash_table_contains(g_items, key)) {
        g_free(key);
        return;
    }

    Item *it = g_new0(Item, 1);
    it->bus  = g_strdup(bus);
    it->path = g_strdup(path);
    it->key  = key;

    it->button = gtk_button_new();
    gtk_widget_add_css_class(it->button, "tray-item");
    gtk_widget_set_focusable(it->button, FALSE);
    it->image = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(it->image), ICON_SIZE);
    gtk_button_set_child(GTK_BUTTON(it->button), it->image);
    g_signal_connect(it->button, "clicked",
                     G_CALLBACK(on_item_clicked), it);

    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    g_signal_connect(rc, "pressed", G_CALLBACK(on_item_right_click), it);
    gtk_widget_add_controller(it->button, GTK_EVENT_CONTROLLER(rc));

    gtk_box_append(GTK_BOX(g_icons_box), it->button);
    g_hash_table_insert(g_items, g_strdup(it->key), it);

    it->signal_sub = g_dbus_connection_signal_subscribe(
        g_bus, it->bus, SNI_ITEM_IFACE, NULL, it->path, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_item_signal, it, NULL);
    it->name_watch = g_bus_watch_name_on_connection(
        g_bus, it->bus, G_BUS_NAME_WATCHER_FLAGS_NONE,
        NULL, on_name_vanished, it, NULL);

    fetch_props(it);
}

static void item_free(Item *it)
{
    if (it->signal_sub)
        g_dbus_connection_signal_unsubscribe(g_bus, it->signal_sub);
    if (it->name_watch)
        g_bus_unwatch_name(it->name_watch);
    if (it->button)
        gtk_box_remove(GTK_BOX(g_icons_box), it->button);
    g_free(it->bus);
    g_free(it->path);
    g_free(it->key);
    g_free(it->menu_path);
    g_free(it);
}

static void remove_item(const char *key)
{
    Item *it = g_hash_table_lookup(g_items, key);
    if (!it)
        return;

    /* Pulisci anche il registro del watcher ed emetti Unregistered. */
    char *svc = g_strdup_printf("%s%s", it->bus, it->path);
    if (g_hash_table_remove(g_registered, svc) && g_is_watcher)
        g_dbus_connection_emit_signal(
            g_bus, NULL, SNI_WATCHER_PATH, SNI_WATCHER_IFACE,
            "StatusNotifierItemUnregistered",
            g_variant_new("(s)", svc), NULL);
    g_free(svc);

    /* stacca dai lookup prima di liberare (i callback lo controllano). */
    g_hash_table_steal(g_items, key);
    item_free(it);
}

/* ---- Watcher (server) --------------------------------------------------- */

static const char WATCHER_XML[] =
    "<node>"
    " <interface name='org.kde.StatusNotifierWatcher'>"
    "  <method name='RegisterStatusNotifierItem'>"
    "   <arg type='s' direction='in'/></method>"
    "  <method name='RegisterStatusNotifierHost'>"
    "   <arg type='s' direction='in'/></method>"
    "  <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "  <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "  <property name='ProtocolVersion' type='i' access='read'/>"
    "  <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierHostRegistered'/>"
    "  <signal name='StatusNotifierHostUnregistered'/>"
    " </interface>"
    "</node>";

static void watcher_method(GDBusConnection *conn G_GNUC_UNUSED,
                           const char *sender, const char *obj G_GNUC_UNUSED,
                           const char *iface G_GNUC_UNUSED,
                           const char *method, GVariant *params,
                           GDBusMethodInvocation *inv, gpointer ud G_GNUC_UNUSED)
{
    if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
        const char *service;
        g_variant_get(params, "(&s)", &service);
        char *bus, *path;
        parse_service(service, sender, &bus, &path);
        char *svc = g_strdup_printf("%s%s", bus, path);

        if (!g_hash_table_contains(g_registered, svc)) {
            g_hash_table_add(g_registered, g_strdup(svc));
            add_item(bus, path);
            g_dbus_connection_emit_signal(
                g_bus, NULL, SNI_WATCHER_PATH, SNI_WATCHER_IFACE,
                "StatusNotifierItemRegistered",
                g_variant_new("(s)", svc), NULL);
        }
        g_free(svc); g_free(bus); g_free(path);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "RegisterStatusNotifierHost") == 0) {
        g_dbus_connection_emit_signal(
            g_bus, NULL, SNI_WATCHER_PATH, SNI_WATCHER_IFACE,
            "StatusNotifierHostRegistered", NULL, NULL);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_error(
            inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no such method");
    }
}

static GVariant *watcher_get_prop(GDBusConnection *c G_GNUC_UNUSED,
                                  const char *sender G_GNUC_UNUSED,
                                  const char *obj G_GNUC_UNUSED,
                                  const char *iface G_GNUC_UNUSED,
                                  const char *prop, GError **err G_GNUC_UNUSED,
                                  gpointer ud G_GNUC_UNUSED)
{
    if (g_strcmp0(prop, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        GHashTableIter it;
        gpointer key;
        g_hash_table_iter_init(&it, g_registered);
        while (g_hash_table_iter_next(&it, &key, NULL))
            g_variant_builder_add(&b, "s", (const char *) key);
        return g_variant_builder_end(&b);
    }
    if (g_strcmp0(prop, "IsStatusNotifierHostRegistered") == 0)
        return g_variant_new_boolean(TRUE);
    if (g_strcmp0(prop, "ProtocolVersion") == 0)
        return g_variant_new_int32(0);
    return NULL;
}

static const GDBusInterfaceVTable WATCHER_VTABLE = {
    watcher_method, watcher_get_prop, NULL, { 0 }
};

static void on_watcher_acquired(GDBusConnection *conn, const char *name,
                                gpointer ud G_GNUC_UNUSED)
{
    /* L'oggetto e' gia' registrato in tray_dbus_init (nessuna race: e'
     * pronto nell'istante in cui le app vedono comparire il nome). */
    g_is_watcher = TRUE;
    g_dbus_connection_emit_signal(conn, NULL, SNI_WATCHER_PATH,
                                  SNI_WATCHER_IFACE,
                                  "StatusNotifierHostRegistered", NULL, NULL);
    (void) name;
}

/* ---- Client mode (watcher gia' presente) -------------------------------- */

static void on_watcher_item_registered(GDBusConnection *c G_GNUC_UNUSED,
                                       const char *s G_GNUC_UNUSED,
                                       const char *p G_GNUC_UNUSED,
                                       const char *i G_GNUC_UNUSED,
                                       const char *sig G_GNUC_UNUSED,
                                       GVariant *params, gpointer ud G_GNUC_UNUSED)
{
    const char *service;
    g_variant_get(params, "(&s)", &service);
    char *bus, *path;
    parse_service(service, NULL, &bus, &path);
    if (*bus)
        add_item(bus, path);
    g_free(bus); g_free(path);
}

static void on_watcher_item_unregistered(GDBusConnection *c G_GNUC_UNUSED,
                                         const char *s G_GNUC_UNUSED,
                                         const char *p G_GNUC_UNUSED,
                                         const char *i G_GNUC_UNUSED,
                                         const char *sig G_GNUC_UNUSED,
                                         GVariant *params, gpointer ud G_GNUC_UNUSED)
{
    const char *service;
    g_variant_get(params, "(&s)", &service);
    char *bus, *path;
    parse_service(service, NULL, &bus, &path);
    char *key = make_key(bus, path);
    remove_item(key);
    g_free(key); g_free(bus); g_free(path);
}

static void on_registered_items(GObject *src, GAsyncResult *res,
                                gpointer ud G_GNUC_UNUSED)
{
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  res, NULL);
    if (!ret)
        return;
    GVariant *box, *arr;
    g_variant_get(ret, "(v)", &box);
    arr = box;
    GVariantIter it;
    const char *service;
    g_variant_iter_init(&it, arr);
    while (g_variant_iter_next(&it, "&s", &service)) {
        char *bus, *path;
        parse_service(service, NULL, &bus, &path);
        if (*bus)
            add_item(bus, path);
        g_free(bus); g_free(path);
    }
    g_variant_unref(box);
    g_variant_unref(ret);
}

static void client_init(void)
{
    g_dbus_connection_signal_subscribe(
        g_bus, NULL, SNI_WATCHER_IFACE, "StatusNotifierItemRegistered",
        NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_watcher_item_registered, NULL, NULL);
    g_dbus_connection_signal_subscribe(
        g_bus, NULL, SNI_WATCHER_IFACE, "StatusNotifierItemUnregistered",
        NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_watcher_item_unregistered, NULL, NULL);

    g_dbus_connection_call(
        g_bus, SNI_WATCHER_NAME, SNI_WATCHER_PATH,
        SNI_WATCHER_IFACE, "RegisterStatusNotifierHost",
        g_variant_new("(s)", g_host_name), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

    g_dbus_connection_call(
        g_bus, SNI_WATCHER_NAME, SNI_WATCHER_PATH,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", SNI_WATCHER_IFACE,
                      "RegisteredStatusNotifierItems"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_registered_items, NULL);
}

static void on_watcher_lost(GDBusConnection *conn G_GNUC_UNUSED,
                            const char *name G_GNUC_UNUSED,
                            gpointer ud G_GNUC_UNUSED)
{
    if (!g_is_watcher)          /* non l'abbiamo mai preso -> client mode */
        client_init();
}

/* ---- Bootstrap ---------------------------------------------------------- */

static void tray_dbus_init(void)
{
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!g_bus)
        return;

    g_items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_registered = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_watcher_info = g_dbus_node_info_new_for_xml(WATCHER_XML, NULL);
    g_host_name = g_strdup_printf("org.kde.StatusNotifierHost-%d", getpid());

    /* Registra subito l'oggetto watcher: cosi' quando prendiamo il nome e
     * le app iniziano a registrarsi, l'handler e' gia' pronto (niente race
     * che faccia perdere le app gia' aperte). */
    g_watcher_obj_id = g_dbus_connection_register_object(
        g_bus, SNI_WATCHER_PATH, g_watcher_info->interfaces[0],
        &WATCHER_VTABLE, NULL, NULL, NULL);

    g_bus_own_name_on_connection(g_bus, g_host_name,
                                 G_BUS_NAME_OWNER_FLAGS_NONE,
                                 NULL, NULL, NULL, NULL);

    g_bus_own_name_on_connection(g_bus, SNI_WATCHER_NAME,
                                 G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_watcher_acquired, on_watcher_lost,
                                 NULL, NULL);
}

/* ---- Toggle / freccia --------------------------------------------------- */

static void on_toggle(GtkButton *btn, gpointer user_data)
{
    GtkRevealer *rev = GTK_REVEALER(user_data);
    gboolean now = !gtk_revealer_get_reveal_child(rev);
    gtk_revealer_set_reveal_child(rev, now);
    if (now) {
        gtk_widget_add_css_class(GTK_WIDGET(btn), "expanded");
        tray_schedule_close();          /* aperto -> parte il conto */
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(btn), "expanded");
        tray_cancel_close();
    }
}

/* Il puntatore entra nel tray (freccia o icone): niente auto-chiusura finche'
 * resta sopra, anche se fermo. */
static void on_tray_enter(GtkEventControllerMotion *c G_GNUC_UNUSED,
                          double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                          gpointer ud G_GNUC_UNUSED)
{
    g_pointer_inside = TRUE;
    tray_cancel_close();
}

/* Il puntatore lascia il tray: avvia il conto alla rovescia. */
static void on_tray_leave(GtkEventControllerMotion *c G_GNUC_UNUSED,
                          gpointer ud G_GNUC_UNUSED)
{
    g_pointer_inside = FALSE;
    tray_schedule_close();
}

GtkWidget *tray_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "tray");
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    g_icons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(g_icons_box, "tray-icons");

    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 200);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    gtk_revealer_set_child(GTK_REVEALER(revealer), g_icons_box);
    g_revealer = revealer;

    GtkWidget *toggle = gtk_button_new();
    gtk_widget_add_css_class(toggle, "tray-toggle");
    gtk_widget_set_focusable(toggle, FALSE);
    GtkWidget *arrow = gtk_image_new_from_icon_name("pan-start-symbolic");
    gtk_widget_add_css_class(arrow, "tray-arrow");
    gtk_button_set_child(GTK_BUTTON(toggle), arrow);
    g_signal_connect(toggle, "clicked", G_CALLBACK(on_toggle), revealer);
    g_toggle = toggle;

    /* icone a sinistra della freccia, freccia adiacente all'orologio */
    gtk_box_append(GTK_BOX(box), revealer);
    gtk_box_append(GTK_BOX(box), toggle);

    /* Entrata/uscita del puntatore sul tray -> sospende/riavvia l'auto-chiusura.
     * enter/leave coprono l'intera area del tray (freccia + icone, figli
     * inclusi): finche' il mouse e' sopra non si ricompatta. */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_tray_enter), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_tray_leave), NULL);
    gtk_widget_add_controller(box, motion);

    tray_dbus_init();
    return box;
}
