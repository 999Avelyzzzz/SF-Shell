#include "workspaces.h"
#include <json-glib/json-glib.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>

#define FIXED_WORKSPACES 5   /* 1..5 sempre visibili, vuoti o no */

typedef struct {
    GtkWidget *box;        /* contenitore dei bottoni workspace */
    char      *cmd_path;   /* .socket.sock  (richiesta/risposta)  */
    char      *evt_path;   /* .socket2.sock (stream di eventi)    */
    GSocket   *evt_sock;   /* tenuta viva: possiede l'fd eventi   */
    GSource   *evt_source;
    GString   *evt_buf;    /* accumulo per righe spezzate         */
} Workspaces;

/* ---- IPC ---------------------------------------------------------------- */

/* Costruisce il path di un socket Hyprland nella dir dell'istanza. */
static char *hypr_socket_path(const char *name)
{
    const char *sig = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig)
        return NULL;
    return g_build_filename(g_get_user_runtime_dir(), "hypr", sig, name, NULL);
}

/* Richiesta sincrona sul socket comandi: invia `request`, ritorna la
 * risposta completa (da liberare con g_free), oppure NULL su errore. */
static char *hypr_request(const char *cmd_path, const char *request)
{
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
    return result;
}

/* ---- Switch al click ---------------------------------------------------- */

static void on_ws_clicked(GtkButton *btn, gpointer user_data)
{
    Workspaces *ws = user_data;
    int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "ws-id"));
    char *req = g_strdup_printf("dispatch workspace %d", id);
    char *reply = hypr_request(ws->cmd_path, req);
    g_free(reply);
    g_free(req);
}

/* ---- Refresh dallo stato ------------------------------------------------ */

static gint int_cmp(gconstpointer a, gconstpointer b)
{
    return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}

/* Estrae l'id del workspace attivo (0 se non determinabile). */
static int query_active_id(Workspaces *ws)
{
    int active = 0;
    char *json = hypr_request(ws->cmd_path, "j/activeworkspace");
    if (!json)
        return 0;

    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, json, -1, NULL)) {
        JsonObject *obj = json_node_get_object(json_parser_get_root(p));
        if (obj && json_object_has_member(obj, "id"))
            active = (int) json_object_get_int_member(obj, "id");
    }
    g_object_unref(p);
    g_free(json);
    return active;
}

static void clear_box(GtkWidget *box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), child);
}

static void add_button(Workspaces *ws, int id, int active, gboolean occupied)
{
    char *label = g_strdup_printf("%d", id);
    GtkWidget *btn = gtk_button_new_with_label(label);
    g_free(label);

    gtk_widget_add_css_class(btn, "ws");
    gtk_widget_add_css_class(btn, occupied ? "occupied" : "empty");
    if (id == active)
        gtk_widget_add_css_class(btn, "active");
    gtk_widget_set_focusable(btn, FALSE);

    g_object_set_data(G_OBJECT(btn), "ws-id", GINT_TO_POINTER(id));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_ws_clicked), ws);

    gtk_box_append(GTK_BOX(ws->box), btn);
}

static void workspaces_refresh(Workspaces *ws)
{
    char *json = hypr_request(ws->cmd_path, "j/workspaces");
    if (!json)
        return;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, NULL)) {
        g_object_unref(parser);
        g_free(json);
        return;
    }

    int active = query_active_id(ws);

    /* Mappa id -> numero finestre per i workspace esistenti (id > 0). */
    GHashTable *windows = g_hash_table_new(g_direct_hash, g_direct_equal);
    JsonArray *arr = json_node_get_array(json_parser_get_root(parser));
    guint len = arr ? json_array_get_length(arr) : 0;
    for (guint i = 0; i < len; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        int id = (int) json_object_get_int_member(o, "id");
        if (id <= 0)                     /* salta i workspace speciali */
            continue;
        int w = (int) json_object_get_int_member(o, "windows");
        g_hash_table_insert(windows, GINT_TO_POINTER(id), GINT_TO_POINTER(w));
    }

    /* Lista di id da mostrare: 1..FIXED sempre, piu' gli esistenti > FIXED. */
    GList *ids = NULL;
    for (int id = 1; id <= FIXED_WORKSPACES; id++)
        ids = g_list_prepend(ids, GINT_TO_POINTER(id));

    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, windows);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        if (GPOINTER_TO_INT(key) > FIXED_WORKSPACES)
            ids = g_list_prepend(ids, key);
    }
    ids = g_list_sort(ids, int_cmp);

    /* Ricostruisce i bottoni. */
    clear_box(ws->box);
    for (GList *l = ids; l; l = l->next) {
        int id = GPOINTER_TO_INT(l->data);
        int w = GPOINTER_TO_INT(g_hash_table_lookup(windows,
                                                    GINT_TO_POINTER(id)));
        add_button(ws, id, active, w > 0);
    }

    g_list_free(ids);
    g_hash_table_destroy(windows);
    g_object_unref(parser);
    g_free(json);
}

/* ---- Socket eventi ------------------------------------------------------ */

/* Eventi Hyprland che richiedono un refresh dei workspace. */
static gboolean event_is_relevant(const char *line)
{
    static const char *const events[] = {
        "workspace", "workspacev2",
        "createworkspace", "createworkspacev2",
        "destroyworkspace", "destroyworkspacev2",
        "moveworkspace", "moveworkspacev2",
        "renameworkspace", "focusedmon", "activespecial",
        "openwindow", "closewindow",   /* cambiano lo stato vuoto/occupato */
        NULL
    };
    for (int i = 0; events[i]; i++) {
        size_t n = strlen(events[i]);
        if (strncmp(line, events[i], n) == 0 && line[n] == '>')
            return TRUE;
    }
    return FALSE;
}

static gboolean on_event_in(GSocket *sock, GIOCondition cond,
                            gpointer user_data)
{
    Workspaces *ws = user_data;

    if (cond & (G_IO_HUP | G_IO_ERR))
        return G_SOURCE_REMOVE;

    char buf[4096];
    gssize n = g_socket_receive(sock, buf, sizeof buf, NULL, NULL);
    if (n <= 0)                          /* 0 = chiuso, <0 = errore */
        return G_SOURCE_REMOVE;
    g_string_append_len(ws->evt_buf, buf, (gsize) n);

    /* Processa tutte le righe complete accumulate. */
    gboolean need_refresh = FALSE;
    char *nl;
    while ((nl = strchr(ws->evt_buf->str, '\n')) != NULL) {
        *nl = '\0';
        if (event_is_relevant(ws->evt_buf->str))
            need_refresh = TRUE;
        g_string_erase(ws->evt_buf, 0, (nl - ws->evt_buf->str) + 1);
    }

    if (need_refresh)
        workspaces_refresh(ws);

    return G_SOURCE_CONTINUE;
}

static void event_socket_connect(Workspaces *ws)
{
    if (!ws->evt_path)
        return;

    GSocketAddress *addr = g_unix_socket_address_new(ws->evt_path);
    GSocket *sock = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                 G_SOCKET_PROTOCOL_DEFAULT, NULL);

    if (sock && g_socket_connect(sock, addr, NULL, NULL)) {
        ws->evt_sock = sock;             /* la teniamo viva: possiede l'fd */
        ws->evt_buf = g_string_new(NULL);
        ws->evt_source = g_socket_create_source(
            sock, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
        g_source_set_callback(ws->evt_source, G_SOURCE_FUNC(on_event_in),
                              ws, NULL);
        g_source_attach(ws->evt_source, NULL);
    } else if (sock) {
        g_object_unref(sock);
    }
    g_object_unref(addr);
}

/* ---- Ciclo di vita ------------------------------------------------------ */

static void workspaces_free(gpointer data, GObject *where G_GNUC_UNUSED)
{
    Workspaces *ws = data;
    if (ws->evt_source) {
        g_source_destroy(ws->evt_source);
        g_source_unref(ws->evt_source);
    }
    if (ws->evt_sock) {
        g_socket_close(ws->evt_sock, NULL);
        g_object_unref(ws->evt_sock);
    }
    if (ws->evt_buf)
        g_string_free(ws->evt_buf, TRUE);
    g_free(ws->cmd_path);
    g_free(ws->evt_path);
    g_free(ws);
}

GtkWidget *workspaces_new(void)
{
    Workspaces *ws = g_new0(Workspaces, 1);
    ws->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(ws->box, "workspaces");
    gtk_widget_set_valign(ws->box, GTK_ALIGN_CENTER);

    ws->cmd_path = hypr_socket_path(".socket.sock");
    ws->evt_path = hypr_socket_path(".socket2.sock");

    workspaces_refresh(ws);
    event_socket_connect(ws);

    g_object_weak_ref(G_OBJECT(ws->box), workspaces_free, ws);
    return ws->box;
}
