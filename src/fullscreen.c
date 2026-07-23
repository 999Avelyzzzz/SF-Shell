#include "fullscreen.h"
#include <gtk4-layer-shell.h>
#include <json-glib/json-glib.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>

/* Una barra registrata: la window, il connector del suo monitor e lo stato
 * corrente (nascosta o no), per evitare set_visible ridondanti. */
typedef struct {
    GtkWindow *bar;
    char      *connector;
    gboolean   hidden;
} BarEntry;

/* Watcher unico condiviso da tutte le barre. */
typedef struct {
    char      *cmd_path;   /* .socket.sock  (richiesta/risposta) */
    char      *evt_path;   /* .socket2.sock (stream di eventi)   */
    GSocket   *evt_sock;   /* tenuta viva: possiede l'fd eventi  */
    GSource   *evt_source;
    GString   *evt_buf;    /* accumulo per righe spezzate        */
    GPtrArray *bars;       /* BarEntry*                          */
} Fullscreen;

static Fullscreen *g_fs;

/* ---- IPC (helper locali, gemelli di quelli in workspaces.c) ------------- */

static char *hypr_socket_path(const char *name)
{
    const char *sig = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig)
        return NULL;
    return g_build_filename(g_get_user_runtime_dir(), "hypr", sig, name, NULL);
}

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

/* ---- Stato fullscreen per-monitor --------------------------------------- */

/* Insieme degli id di workspace che ospitano una finestra a schermo intero
 * "vero" (mode 2). La massimizzazione (mode 1) rispetta lo spazio della barra,
 * quindi non conta. Ritorna una GHashTable<int,gboolean> (chiavi = id ws). */
static GHashTable *fullscreen_workspaces(Fullscreen *fs)
{
    GHashTable *set = g_hash_table_new(g_direct_hash, g_direct_equal);
    char *json = hypr_request(fs->cmd_path, "j/clients");
    if (!json)
        return set;

    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        JsonArray *arr = root && JSON_NODE_HOLDS_ARRAY(root)
                         ? json_node_get_array(root) : NULL;
        guint len = arr ? json_array_get_length(arr) : 0;
        for (guint i = 0; i < len; i++) {
            JsonObject *o = json_array_get_object_element(arr, i);
            if (!o || !json_object_has_member(o, "fullscreen"))
                continue;
            if (json_object_get_int_member(o, "fullscreen") < 2)
                continue;
            JsonObject *ws = json_object_has_member(o, "workspace")
                ? json_object_get_object_member(o, "workspace") : NULL;
            if (ws && json_object_has_member(ws, "id")) {
                int id = (int) json_object_get_int_member(ws, "id");
                g_hash_table_add(set, GINT_TO_POINTER(id));
            }
        }
    }
    g_object_unref(p);
    g_free(json);
    return set;
}

/* Mappa connector(monitor) -> gboolean "deve nascondere la barra". Costruita
 * incrociando i monitor (workspace attivo di ciascuno) con l'insieme dei
 * workspace fullscreen. Ritorna GHashTable<string,gboolean> da distruggere. */
static GHashTable *hidden_by_connector(Fullscreen *fs)
{
    GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, NULL);
    GHashTable *full_ws = fullscreen_workspaces(fs);

    char *json = hypr_request(fs->cmd_path, "j/monitors");
    if (json) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, json, -1, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            JsonArray *arr = root && JSON_NODE_HOLDS_ARRAY(root)
                             ? json_node_get_array(root) : NULL;
            guint len = arr ? json_array_get_length(arr) : 0;
            for (guint i = 0; i < len; i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                if (!o || !json_object_has_member(o, "name"))
                    continue;
                const char *name = json_object_get_string_member(o, "name");
                JsonObject *aw = json_object_has_member(o, "activeWorkspace")
                    ? json_object_get_object_member(o, "activeWorkspace") : NULL;
                int wsid = (aw && json_object_has_member(aw, "id"))
                    ? (int) json_object_get_int_member(aw, "id") : 0;
                gboolean hide = g_hash_table_contains(full_ws,
                                                      GINT_TO_POINTER(wsid));
                g_hash_table_insert(out, g_strdup(name),
                                    GINT_TO_POINTER(hide));
            }
        }
        g_object_unref(p);
        g_free(json);
    }

    g_hash_table_destroy(full_ws);
    return out;
}

/* Ricalcola lo stato e aggiorna la visibilita' di ogni barra registrata. */
static void apply_state(Fullscreen *fs)
{
    if (!fs->bars || fs->bars->len == 0)
        return;

    GHashTable *hide = hidden_by_connector(fs);
    for (guint i = 0; i < fs->bars->len; i++) {
        BarEntry *e = fs->bars->pdata[i];
        gpointer v = NULL;
        gboolean want_hidden = FALSE;
        /* Se il connector non compare (monitor non trovato) resta visibile. */
        if (e->connector &&
            g_hash_table_lookup_extended(hide, e->connector, NULL, &v))
            want_hidden = GPOINTER_TO_INT(v);
        if (want_hidden != e->hidden) {
            e->hidden = want_hidden;
            gtk_widget_set_visible(GTK_WIDGET(e->bar), !want_hidden);
        }
    }
    g_hash_table_destroy(hide);
}

/* ---- Socket eventi ------------------------------------------------------ */

/* Eventi Hyprland dopo i quali lo stato fullscreen puo' essere cambiato. */
static gboolean event_is_relevant(const char *line)
{
    static const char *const events[] = {
        "fullscreen",
        "activewindow", "activewindowv2",
        "workspace", "workspacev2",
        "focusedmon", "openwindow", "closewindow", "movewindowv2",
        "changefloatingmode",
        "monitoradded", "monitorremoved",
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
    Fullscreen *fs = user_data;

    if (cond & (G_IO_HUP | G_IO_ERR))
        return G_SOURCE_REMOVE;

    char buf[4096];
    gssize n = g_socket_receive(sock, buf, sizeof buf, NULL, NULL);
    if (n <= 0)
        return G_SOURCE_REMOVE;
    g_string_append_len(fs->evt_buf, buf, (gsize) n);

    gboolean need_update = FALSE;
    char *nl;
    while ((nl = strchr(fs->evt_buf->str, '\n')) != NULL) {
        *nl = '\0';
        if (event_is_relevant(fs->evt_buf->str))
            need_update = TRUE;
        g_string_erase(fs->evt_buf, 0, (nl - fs->evt_buf->str) + 1);
    }

    if (need_update)
        apply_state(fs);

    return G_SOURCE_CONTINUE;
}

static void event_socket_connect(Fullscreen *fs)
{
    if (!fs->evt_path)
        return;

    GSocketAddress *addr = g_unix_socket_address_new(fs->evt_path);
    GSocket *sock = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                 G_SOCKET_PROTOCOL_DEFAULT, NULL);

    if (sock && g_socket_connect(sock, addr, NULL, NULL)) {
        fs->evt_sock = sock;
        fs->evt_buf = g_string_new(NULL);
        fs->evt_source = g_socket_create_source(
            sock, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
        g_source_set_callback(fs->evt_source, G_SOURCE_FUNC(on_event_in),
                              fs, NULL);
        g_source_attach(fs->evt_source, NULL);
    } else if (sock) {
        g_object_unref(sock);
    }
    g_object_unref(addr);
}

/* ---- Ciclo di vita ------------------------------------------------------ */

static Fullscreen *fullscreen_ensure(void)
{
    if (g_fs)
        return g_fs;
    g_fs = g_new0(Fullscreen, 1);
    g_fs->cmd_path = hypr_socket_path(".socket.sock");
    g_fs->evt_path = hypr_socket_path(".socket2.sock");
    g_fs->bars     = g_ptr_array_new();
    event_socket_connect(g_fs);
    return g_fs;
}

static void bar_entry_free(BarEntry *e)
{
    g_free(e->connector);
    g_free(e);
}

/* La barra e' stata distrutta: rimuovi la sua entry. */
static void on_bar_destroyed(gpointer data, GObject *where)
{
    Fullscreen *fs = g_fs;
    if (!fs)
        return;
    for (guint i = 0; i < fs->bars->len; i++) {
        BarEntry *e = fs->bars->pdata[i];
        if (e->bar == (GtkWindow *) where) {
            bar_entry_free(e);
            g_ptr_array_remove_index_fast(fs->bars, i);
            break;
        }
    }
    (void) data;
}

void fullscreen_register(GtkWindow *bar, const char *connector)
{
    Fullscreen *fs = fullscreen_ensure();

    BarEntry *e = g_new0(BarEntry, 1);
    e->bar       = bar;
    e->connector = g_strdup(connector);
    e->hidden    = FALSE;
    g_ptr_array_add(fs->bars, e);

    g_object_weak_ref(G_OBJECT(bar), on_bar_destroyed, NULL);

    apply_state(fs);            /* stato iniziale di questa barra */
}
