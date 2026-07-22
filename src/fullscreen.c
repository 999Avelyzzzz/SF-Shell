#include "fullscreen.h"
#include <gtk4-layer-shell.h>
#include <json-glib/json-glib.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>

typedef struct {
    GtkWindow *bar;
    char      *cmd_path;   /* .socket.sock  (richiesta/risposta) */
    char      *evt_path;   /* .socket2.sock (stream di eventi)   */
    GSocket   *evt_sock;   /* tenuta viva: possiede l'fd eventi  */
    GSource   *evt_source;
    GString   *evt_buf;    /* accumulo per righe spezzate        */
    gboolean   hidden;     /* stato corrente della barra         */
} Fullscreen;

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

/* ---- Stato fullscreen --------------------------------------------------- */

/* TRUE se la finestra attiva e' a schermo intero "vero" (mode 2). La
 * massimizzazione (mode 1) rispetta lo spazio riservato alla barra, quindi non
 * la nasconde. Se non c'e' finestra attiva, activewindow risponde "{}" e
 * l'assenza del campo "fullscreen" vale 0 -> non fullscreen. */
static gboolean active_is_fullscreen(Fullscreen *fs)
{
    char *json = hypr_request(fs->cmd_path, "j/activewindow");
    if (!json)
        return FALSE;

    gboolean full = FALSE;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (obj && json_object_has_member(obj, "fullscreen"))
                full = json_object_get_int_member(obj, "fullscreen") >= 2;
        }
    }
    g_object_unref(p);
    g_free(json);
    return full;
}

/* Mostra/nasconde la barra rimappando la layer-surface: nascondendola libera
 * anche la exclusive zone, cosi' l'app riottiene tutto lo schermo. */
static void apply_state(Fullscreen *fs)
{
    gboolean want_hidden = active_is_fullscreen(fs);
    if (want_hidden == fs->hidden)
        return;
    fs->hidden = want_hidden;
    gtk_widget_set_visible(GTK_WIDGET(fs->bar), !want_hidden);
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

static void fullscreen_free(gpointer data, GObject *where G_GNUC_UNUSED)
{
    Fullscreen *fs = data;
    if (fs->evt_source) {
        g_source_destroy(fs->evt_source);
        g_source_unref(fs->evt_source);
    }
    if (fs->evt_sock) {
        g_socket_close(fs->evt_sock, NULL);
        g_object_unref(fs->evt_sock);
    }
    if (fs->evt_buf)
        g_string_free(fs->evt_buf, TRUE);
    g_free(fs->cmd_path);
    g_free(fs->evt_path);
    g_free(fs);
}

void fullscreen_watch(GtkWindow *bar)
{
    Fullscreen *fs = g_new0(Fullscreen, 1);
    fs->bar      = bar;
    fs->cmd_path = hypr_socket_path(".socket.sock");
    fs->evt_path = hypr_socket_path(".socket2.sock");

    apply_state(fs);            /* stato iniziale */
    event_socket_connect(fs);

    g_object_weak_ref(G_OBJECT(bar), fullscreen_free, fs);
}
