#include "launcher.h"
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <string.h>

/* ========================================================================
 *  App launcher stile Launchpad (macOS Tahoe):
 *  - bottone "cerchietto" nella barra
 *  - popup overlay a tutto schermo con velo scuro
 *  - pannello centrale traslucido: barra di ricerca + griglia 6x4
 *  Le app arrivano dai .desktop (GAppInfo). Ricerca live che filtra la
 *  griglia; click / Invio lancia e chiude.
 * ======================================================================== */

#define GRID_COLS 6
#define GRID_ROWS 4
#define CELL_W    96
#define CELL_H    104

/* Durata dell'animazione di chiusura (deve coprire la transizione CSS
 * piu' lunga tra velo e pannello). */
#define LAUNCHER_CLOSE_MS 340

static GtkWidget *l_popup;    /* finestra layer-shell overlay (una sola) */
static GtkWidget *l_backdrop; /* velo scuro a tutto schermo              */
static GtkWidget *l_panel;    /* pannello centrale (ricerca + griglia)   */
static GtkWidget *l_search;   /* GtkSearchEntry                          */
static GtkWidget *l_grid;     /* GtkFlowBox con le celle app             */
static char      *l_query;    /* testo di ricerca (lowercase)            */
static guint      l_close_timer; /* timer del fade-out di chiusura        */

/* ---- Lancio app --------------------------------------------------------- */

/* Fine del fade-out: nasconde davvero la finestra e ripulisce lo stato. */
static void launcher_do_hide(gpointer u G_GNUC_UNUSED)
{
    l_close_timer = 0;
    if (l_popup)
        gtk_widget_set_visible(l_popup, FALSE);
    if (l_backdrop)
        gtk_widget_remove_css_class(l_backdrop, "closing");
    if (l_panel)
        gtk_widget_remove_css_class(l_panel, "closing");
}

/* Chiude con animazione: velo in fade-out, pannello che rimpicciolisce dal
 * centro; la finestra viene nascosta a transizione finita. */
static void launcher_hide(void)
{
    if (!l_popup || !gtk_widget_get_visible(l_popup) || l_close_timer)
        return;
    gtk_widget_add_css_class(l_backdrop, "closing");
    gtk_widget_add_css_class(l_panel, "closing");
    l_close_timer = g_timeout_add_once(LAUNCHER_CLOSE_MS,
                                       launcher_do_hide, NULL);
}

static void launch_app(GAppInfo *app)
{
    if (!app)
        return;
    GdkDisplay *display = gdk_display_get_default();
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(display);
    g_app_info_launch(app, NULL, G_APP_LAUNCH_CONTEXT(ctx), NULL);
    g_object_unref(ctx);
    launcher_hide();
}

static void on_child_activated(GtkFlowBox *box G_GNUC_UNUSED,
                               GtkFlowBoxChild *child, gpointer u G_GNUC_UNUSED)
{
    launch_app(g_object_get_data(G_OBJECT(child), "app"));
}

/* ---- Ricerca ------------------------------------------------------------ */

static gboolean filter_cell(GtkFlowBoxChild *child, gpointer u G_GNUC_UNUSED)
{
    if (!l_query || !*l_query)
        return TRUE;
    const char *name = g_object_get_data(G_OBJECT(child), "name-lc");
    return name && strstr(name, l_query) != NULL;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer u G_GNUC_UNUSED)
{
    g_free(l_query);
    l_query = g_utf8_strdown(gtk_editable_get_text(GTK_EDITABLE(entry)), -1);
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(l_grid));
}

/* Invio nella ricerca: lancia la prima app visibile. */
static void on_search_activate(GtkSearchEntry *entry G_GNUC_UNUSED,
                               gpointer u G_GNUC_UNUSED)
{
    for (int i = 0; ; i++) {
        GtkFlowBoxChild *c =
            gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(l_grid), i);
        if (!c)
            break;
        if (gtk_widget_get_child_visible(GTK_WIDGET(c))) {
            launch_app(g_object_get_data(G_OBJECT(c), "app"));
            return;
        }
    }
}

/* ---- Popolamento griglia ------------------------------------------------ */

static int cmp_app_name(gconstpointer a, gconstpointer b)
{
    const char *na = g_app_info_get_display_name(G_APP_INFO(a));
    const char *nb = g_app_info_get_display_name(G_APP_INFO(b));
    return g_utf8_collate(na ? na : "", nb ? nb : "");
}

static GtkWidget *make_cell(GAppInfo *app)
{
    const char *name = g_app_info_get_display_name(app);

    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(cell, "launcher-cell");
    gtk_widget_set_size_request(cell, CELL_W, CELL_H);

    GtkWidget *img = gtk_image_new();
    gtk_widget_add_css_class(img, "launcher-icon");
    GIcon *icon = g_app_info_get_icon(app);
    if (icon)
        gtk_image_set_from_gicon(GTK_IMAGE(img), icon);
    else
        gtk_image_set_from_icon_name(GTK_IMAGE(img),
                                     "application-x-executable");
    gtk_image_set_pixel_size(GTK_IMAGE(img), 56);
    gtk_box_append(GTK_BOX(cell), img);

    GtkWidget *label = gtk_label_new(name ? name : "");
    gtk_widget_add_css_class(label, "launcher-label");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 12);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(cell), label);

    return cell;
}

static void populate_grid(void)
{
    GList *apps = g_app_info_get_all();
    apps = g_list_sort(apps, cmp_app_name);

    for (GList *l = apps; l; l = l->next) {
        GAppInfo *app = G_APP_INFO(l->data);
        if (!g_app_info_should_show(app))
            continue;

        GtkWidget *cell = make_cell(app);
        gtk_flow_box_append(GTK_FLOW_BOX(l_grid), cell);

        /* Il child creato dal FlowBox: ci appendiamo app + nome lowercase. */
        GtkFlowBoxChild *child =
            GTK_FLOW_BOX_CHILD(gtk_widget_get_parent(cell));
        gtk_widget_add_css_class(GTK_WIDGET(child), "launcher-cell-wrap");
        g_object_set_data_full(G_OBJECT(child), "app",
                               g_object_ref(app), g_object_unref);
        const char *name = g_app_info_get_display_name(app);
        g_object_set_data_full(G_OBJECT(child), "name-lc",
                               g_utf8_strdown(name ? name : "", -1), g_free);
    }

    g_list_free_full(apps, g_object_unref);
}

/* ---- Chiusura (Escape / click fuori dal pannello) ----------------------- */

static gboolean on_key(GtkEventControllerKey *c G_GNUC_UNUSED, guint keyval,
                       guint code G_GNUC_UNUSED, GdkModifierType m G_GNUC_UNUSED,
                       gpointer u G_GNUC_UNUSED)
{
    if (keyval == GDK_KEY_Escape) {
        launcher_hide();
        return TRUE;
    }
    return FALSE;
}

/* Click sul velo: chiude solo se il punto e' FUORI dal pannello. */
static void on_backdrop_click(GtkGestureClick *g G_GNUC_UNUSED, int n G_GNUC_UNUSED,
                              double x, double y, gpointer panel)
{
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(panel),
                                   gtk_widget_get_parent(GTK_WIDGET(panel)), &b))
        return;
    if (!graphene_rect_contains_point(&b, &GRAPHENE_POINT_INIT((float) x,
                                                               (float) y)))
        launcher_hide();
}

/* Rotellina verticale -> scroll orizzontale (le app vanno verso destra). */
static gboolean on_scroll(GtkEventControllerScroll *c G_GNUC_UNUSED,
                          double dx, double dy, gpointer scroll)
{
    GtkAdjustment *h =
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll));
    if (!h)
        return FALSE;
    double delta = (dy != 0.0) ? dy : dx;
    double step = gtk_adjustment_get_step_increment(h);
    if (step <= 0.0)
        step = 40.0;
    gtk_adjustment_set_value(h, gtk_adjustment_get_value(h) + delta * step * 3.0);
    return TRUE;
}

/* ---- Costruzione popup -------------------------------------------------- */

static void build_popup(void)
{
    l_popup = gtk_window_new();
    gtk_widget_add_css_class(l_popup, "launcher-popup");

    gtk_layer_init_for_window(GTK_WINDOW(l_popup));
    gtk_layer_set_layer(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(l_popup), "ashell-launcher");
    gtk_layer_set_anchor(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(l_popup),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    /* Velo scuro a tutto schermo (sotto il pannello, e sotto la barra: la
     * finestra overlay rispetta l'exclusive zone della barra e parte sotto
     * di essa). Colore/opacita' da ashell.conf; si espande dal centro. */
    GtkWidget *backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    l_backdrop = backdrop;
    gtk_widget_add_css_class(backdrop, "launcher-backdrop");
    gtk_widget_add_css_class(backdrop, "opening");
    gtk_window_set_child(GTK_WINDOW(l_popup), backdrop);

    /* Pannello centrale traslucido: vexpand+valign CENTER lo mette al
     * centro verticale (un GtkBox altrimenti impacchetta in alto). */
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    l_panel = panel;
    gtk_widget_add_css_class(panel, "launcher-panel");
    gtk_widget_add_css_class(panel, "opening");
    gtk_widget_set_halign(panel, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(panel, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(panel, TRUE);
    gtk_widget_set_vexpand(panel, TRUE);
    gtk_box_append(GTK_BOX(backdrop), panel);

    /* Barra di ricerca. */
    l_search = gtk_search_entry_new();
    gtk_widget_add_css_class(l_search, "launcher-search");
    gtk_widget_set_hexpand(l_search, TRUE);
    gtk_box_append(GTK_BOX(panel), l_search);
    g_signal_connect(l_search, "search-changed",
                     G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(l_search, "activate",
                     G_CALLBACK(on_search_activate), NULL);

    /* Unico separatore tra ricerca e griglia. */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(sep, "launcher-sep");
    gtk_box_append(GTK_BOX(panel), sep);

    /* Griglia a 4 righe FISSE che cresce verso destra: si scrolla in
     * orizzontale (flowbox in orientamento VERTICALE = righe per colonna). */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_size_request(scroll,
                                GRID_COLS * (CELL_W + 14) + (GRID_COLS - 1) * 10,
                                GRID_ROWS * (CELL_H + 14) + (GRID_ROWS - 1) * 10);
    gtk_box_append(GTK_BOX(panel), scroll);

    l_grid = gtk_flow_box_new();
    gtk_widget_add_css_class(l_grid, "launcher-grid");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(l_grid),
                                   GTK_ORIENTATION_VERTICAL);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(l_grid), GRID_ROWS);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(l_grid), GRID_ROWS);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(l_grid), TRUE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(l_grid), 10);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(l_grid), 10);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(l_grid), GTK_SELECTION_NONE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(l_grid), TRUE);
    gtk_flow_box_set_filter_func(GTK_FLOW_BOX(l_grid),
                                 filter_cell, NULL, NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), l_grid);
    g_signal_connect(l_grid, "child-activated",
                     G_CALLBACK(on_child_activated), NULL);

    /* Rotellina -> scroll orizzontale. */
    GtkEventController *sc = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), scroll);
    gtk_widget_add_controller(scroll, sc);

    populate_grid();

    /* Escape chiude. */
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(l_popup, key);

    /* Click sul velo (fuori dal pannello) chiude. */
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed",
                     G_CALLBACK(on_backdrop_click), panel);
    gtk_widget_add_controller(backdrop, GTK_EVENT_CONTROLLER(click));
}

/* ---- Toggle ------------------------------------------------------------- */

/* Toglie lo stato ".opening": le transizioni CSS animano velo e pannello
 * dallo stato ridotto (dal centro) a quello pieno. Chiamata un frame dopo
 * il present, cosi' lo stato iniziale viene disegnato almeno una volta. */
static void open_anim_cb(gpointer u G_GNUC_UNUSED)
{
    if (l_backdrop)
        gtk_widget_remove_css_class(l_backdrop, "opening");
    if (l_panel)
        gtk_widget_remove_css_class(l_panel, "opening");
}

static void on_toggle(GtkButton *b G_GNUC_UNUSED, gpointer u G_GNUC_UNUSED)
{
    if (!l_popup)
        build_popup();

    /* Visibile e non in chiusura -> chiudi (con fade-out). */
    if (gtk_widget_get_visible(l_popup) && !l_close_timer) {
        launcher_hide();
        return;
    }

    /* Apertura: annulla un'eventuale chiusura in corso. */
    if (l_close_timer) {
        g_source_remove(l_close_timer);
        l_close_timer = 0;
    }
    gtk_widget_remove_css_class(l_backdrop, "closing");
    gtk_widget_remove_css_class(l_panel, "closing");

    /* Reset stato e apri con animazione dal centro (pannello scala,
     * velo in fade). */
    gtk_editable_set_text(GTK_EDITABLE(l_search), "");
    g_free(l_query);
    l_query = NULL;
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(l_grid));

    gtk_widget_add_css_class(l_backdrop, "opening");
    gtk_widget_add_css_class(l_panel, "opening");
    gtk_window_present(GTK_WINDOW(l_popup));
    gtk_widget_grab_focus(l_search);
    g_timeout_add_once(40, open_anim_cb, NULL);
}

static void test_open_once(gpointer btn)
{
    on_toggle(GTK_BUTTON(btn), NULL);
}

GtkWidget *launcher_button_new(void)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "launcher-btn");
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);

    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "launcher-dot");
    gtk_button_set_child(GTK_BUTTON(btn), dot);

    g_signal_connect(btn, "clicked", G_CALLBACK(on_toggle), NULL);

    /* Hook di debug: apre il launcher all'avvio se ASHELL_LAUNCHER_TEST=1. */
    if (g_strcmp0(g_getenv("ASHELL_LAUNCHER_TEST"), "1") == 0)
        g_timeout_add_once(600, test_open_once, btn);

    return btn;
}
