#include "launcher.h"
#include "fuzzy.h"
#include "config.h"
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <string.h>
#include <math.h>

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

/* Tetto ai risultati mostrati durante una ricerca (primi N per punteggio). */
#define LAUNCHER_MAX_RESULTS 8

/* Rilevanza: si mostrano solo i risultati "veri". Soglia assoluta (sotto =
 * rumore, non un match) e finestra relativa al miglior punteggio (mostra le
 * alternative vicine al migliore, es. chrome/chromium). Valori tarabili. */
#define LAUNCHER_MIN_SCORE 0.30
#define LAUNCHER_REL_GAP   0.40

/* Dati precalcolati per cella: nomi in minuscolo (per il match fuzzy) e
 * l'ultimo punteggio calcolato. */
typedef struct {
    char  *name_lc;
    char  *id_lc;
    double score;
} Cell;

static void cell_free(gpointer p)
{
    Cell *c = p;
    g_free(c->name_lc);
    g_free(c->id_lc);
    g_free(c);
}

/* Durata dell'animazione di chiusura (deve coprire la transizione CSS
 * piu' lunga tra velo e pannello). */
#define LAUNCHER_CLOSE_MS 340

static GtkWidget *l_popup;    /* finestra layer-shell overlay (una sola) */
static GtkWidget *l_backdrop; /* velo scuro a tutto schermo              */
static GtkWidget *l_blur_pic; /* wallpaper sfocato dietro il pannello    */
static GtkWidget *l_panel;    /* pannello centrale (ricerca + griglia)   */
static GtkWidget *l_search;   /* GtkSearchEntry                          */
static GtkWidget *l_grid;     /* GtkFlowBox con le celle app             */
static GtkWidget *l_scroll;   /* GtkScrolledWindow che contiene la griglia */
static GtkWidget *l_button;   /* tasto nella barra (per lo stato active) */
static char      *l_query;    /* testo di ricerca (lowercase)            */
static guint      l_close_timer; /* timer del fade-out di chiusura        */

/* Stato della ricerca fuzzy. */
static gboolean         l_searching;  /* TRUE se c'e' un pattern in corso  */
static double           l_cutoff;     /* rivela le celle con score >= cutoff */
static GtkFlowBoxChild *l_best;       /* miglior risultato (per Invio)      */

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
    if (l_button)
        gtk_widget_remove_css_class(l_button, "active");
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

/* Mostra la cella solo se il suo punteggio rientra tra i migliori (>= cutoff).
 * Senza pattern il cutoff e' negativo, quindi si vedono tutte. */
static gboolean filter_cell(GtkFlowBoxChild *child, gpointer u G_GNUC_UNUSED)
{
    Cell *c = g_object_get_data(G_OBJECT(child), "cell");
    return c && c->score >= l_cutoff;
}

/* Ordina: durante la ricerca per punteggio decrescente (miglior match in
 * testa), altrimenti in ordine alfabetico. */
static int sort_cell(GtkFlowBoxChild *a, GtkFlowBoxChild *b,
                     gpointer u G_GNUC_UNUSED)
{
    Cell *ca = g_object_get_data(G_OBJECT(a), "cell");
    Cell *cb = g_object_get_data(G_OBJECT(b), "cell");
    if (!ca || !cb)
        return 0;
    if (l_searching) {
        if (cb->score > ca->score) return 1;
        if (cb->score < ca->score) return -1;
    }
    return g_utf8_collate(ca->name_lc, cb->name_lc);
}

static int cmp_double_desc(const void *a, const void *b)
{
    double da = *(const double *) a, db = *(const double *) b;
    return (db > da) - (db < da);
}

/* Punteggio fuzzy dell'app rispetto al pattern: massimo tra nome e app-id,
 * come Application.match() di material-you (le keyword la' sono penalizzate a
 * tal punto da non contare mai, quindi qui bastano nome e id). */
static double cell_match(const Cell *c, const char *pattern)
{
    double s = fuzzy_score(c->name_lc, pattern);
    if (c->id_lc) {
        double sid = fuzzy_score(c->id_lc, pattern);
        if (sid > s) s = sid;
    }
    return s;
}

/* Dispone la griglia in base al numero di risultati visibili:
 *  - n <= 0 (nessuna ricerca): griglia piena 4-righe che scorre in orizzontale;
 *  - in ricerca: blocco CENTRATO nel launcher, celle della solita dimensione.
 *    Colonne = n fino a 3, poi ceil(sqrt(n)) per una forma piu' quadrata:
 *      1->1, 2->2, 3->3, 4->2x2, 5->3+2, 6->3x2, ... sempre centrate. */
static void apply_result_layout(int n)
{
    GtkFlowBox *fb = GTK_FLOW_BOX(l_grid);
    if (n <= 0) {
        gtk_orientable_set_orientation(GTK_ORIENTABLE(fb),
                                       GTK_ORIENTATION_VERTICAL);
        gtk_flow_box_set_min_children_per_line(fb, GRID_ROWS);
        gtk_flow_box_set_max_children_per_line(fb, GRID_ROWS);
        gtk_widget_set_halign(l_grid, GTK_ALIGN_FILL);
        gtk_widget_set_valign(l_grid, GTK_ALIGN_FILL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(l_scroll),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    } else {
        int cols = (n <= 3) ? n : (int) ceil(sqrt((double) n));
        gtk_orientable_set_orientation(GTK_ORIENTABLE(fb),
                                       GTK_ORIENTATION_HORIZONTAL);
        gtk_flow_box_set_min_children_per_line(fb, cols);
        gtk_flow_box_set_max_children_per_line(fb, cols);
        gtk_widget_set_halign(l_grid, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(l_grid, GTK_ALIGN_CENTER);
        /* Pochi risultati: niente scroll, cosi' il blocco resta centrato. */
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(l_scroll),
                                       GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    }
}

/* Azzera lo stato di ricerca: tutte le celle visibili, ordine alfabetico. */
static void search_reset(void)
{
    l_searching = FALSE;
    l_cutoff = -1.0;
    l_best = NULL;
    for (int i = 0; ; i++) {
        GtkFlowBoxChild *ch =
            gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(l_grid), i);
        if (!ch) break;
        Cell *c = g_object_get_data(G_OBJECT(ch), "cell");
        if (c) c->score = 0.0;
    }
    apply_result_layout(0);
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(l_grid));
    gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(l_grid));
}

static void on_search_changed(GtkSearchEntry *entry, gpointer u G_GNUC_UNUSED)
{
    g_free(l_query);
    l_query = g_utf8_strdown(gtk_editable_get_text(GTK_EDITABLE(entry)), -1);

    char *pat = g_strstrip(g_strdup(l_query ? l_query : ""));
    if (!*pat) {
        g_free(pat);
        search_reset();
        return;
    }

    l_searching = TRUE;
    l_best = NULL;

    /* Calcola il punteggio di ogni cella e raccogli i valori per la soglia. */
    GArray *scores = g_array_new(FALSE, FALSE, sizeof(double));
    double best_score = -1.0;
    for (int i = 0; ; i++) {
        GtkFlowBoxChild *ch =
            gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(l_grid), i);
        if (!ch) break;
        Cell *c = g_object_get_data(G_OBJECT(ch), "cell");
        if (!c) continue;
        c->score = cell_match(c, pat);
        g_array_append_val(scores, c->score);
        if (c->score > best_score) {
            best_score = c->score;
            l_best = ch;
        }
    }

    /* Punteggio che delimita i primi N (tetto massimo di risultati). */
    double eighth = 0.0;
    if (scores->len > 0) {
        qsort(scores->data, scores->len, sizeof(double), cmp_double_desc);
        guint k = MIN((guint) LAUNCHER_MAX_RESULTS, scores->len);
        eighth = g_array_index(scores, double, k - 1);
    }
    g_array_free(scores, TRUE);
    g_free(pat);

    /* Soglia finale = max(soglia assoluta, migliore - finestra, tetto top-N).
     * Se nemmeno il migliore raggiunge la soglia assoluta, non passa nulla. */
    l_cutoff = LAUNCHER_MIN_SCORE;
    double rel = best_score - LAUNCHER_REL_GAP;
    if (rel > l_cutoff)    l_cutoff = rel;
    if (eighth > l_cutoff) l_cutoff = eighth;

    /* Conta i risultati che passano, per centrarli con la forma giusta. */
    int nvis = 0;
    for (int i = 0; ; i++) {
        GtkFlowBoxChild *ch =
            gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(l_grid), i);
        if (!ch) break;
        Cell *c = g_object_get_data(G_OBJECT(ch), "cell");
        if (c && c->score >= l_cutoff) nvis++;
    }

    apply_result_layout(nvis);
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(l_grid));
    gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(l_grid));
}

/* Invio nella ricerca: lancia il miglior risultato (o la prima cella visibile). */
static void on_search_activate(GtkSearchEntry *entry G_GNUC_UNUSED,
                               gpointer u G_GNUC_UNUSED)
{
    if (l_best && gtk_widget_get_child_visible(GTK_WIDGET(l_best))) {
        launch_app(g_object_get_data(G_OBJECT(l_best), "app"));
        return;
    }
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

        /* Il child creato dal FlowBox: ci appendiamo app + dati per il match. */
        GtkFlowBoxChild *child =
            GTK_FLOW_BOX_CHILD(gtk_widget_get_parent(cell));
        gtk_widget_add_css_class(GTK_WIDGET(child), "launcher-cell-wrap");
        g_object_set_data_full(G_OBJECT(child), "app",
                               g_object_ref(app), g_object_unref);

        Cell *c = g_new0(Cell, 1);
        const char *name = g_app_info_get_display_name(app);
        c->name_lc = g_utf8_strdown(name ? name : "", -1);
        const char *id = g_app_info_get_id(app);
        c->id_lc = id ? g_utf8_strdown(id, -1) : NULL;
        g_object_set_data_full(G_OBJECT(child), "cell", c, cell_free);
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

/* ---- Blur universale (frosted glass) ------------------------------------ */

/* GTK4 non ha backdrop-filter e non esiste un protocollo Wayland universale per
 * sfocare cio' che sta DIETRO una surface (Hyprland usa le layerrule, KDE un
 * suo protocollo: nessuno portabile). L'unico metodo identico su OGNI
 * compositor (e pure X11) e' sfocare NOI il contenuto e disegnarlo dentro la
 * nostra surface: prendiamo il wallpaper e ne mostriamo una copia sfocata come
 * sfondo del launcher. Il pannello semitrasparente sopra lascia trasparire la
 * sfocatura = effetto vetro smerigliato. Limite: sfoca il wallpaper, non le
 * finestre dietro (irrilevante per un overlay a tutto schermo che le copre). */

/* Un passaggio di box blur separabile (orizzontale o verticale) da src a dst.
 * Piu' passaggi ripetuti approssimano una gaussiana. Clampa ai bordi. */
static void blur_pass(const guchar *src, guchar *dst,
                      int w, int h, int ch, int rowstride,
                      int radius, gboolean horizontal)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int acc[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (int k = -radius; k <= radius; k++) {
                int xx = horizontal ? x + k : x;
                int yy = horizontal ? y : y + k;
                if (xx < 0 || xx >= w || yy < 0 || yy >= h)
                    continue;
                const guchar *p = src + yy * rowstride + xx * ch;
                for (int c = 0; c < ch; c++)
                    acc[c] += p[c];
                cnt++;
            }
            guchar *o = dst + y * rowstride + x * ch;
            for (int c = 0; c < ch; c++)
                o[c] = (guchar) (acc[c] / cnt);
        }
    }
}

/* Sceglie un path di wallpaper dalla config (generico, poi esteso, poi il primo
 * per-monitor). Ritorna un puntatore di proprieta' della config (non liberare). */
static const char *pick_wallpaper_path(void)
{
    const char *p = config_get("wallpaper");
    if (p) return p;
    p = config_get("wallpaper-extended");
    if (p) return p;

    char **keys = config_keys_with_prefix("wallpaper-");
    const char *found = NULL;
    for (int i = 0; keys && keys[i]; i++) {
        const char *v = config_get(keys[i]);
        if (v) { found = v; break; }   /* v resta valido: appartiene alla config */
    }
    g_strfreev(keys);
    return found;
}

/* Costruisce una texture del wallpaper corrente, sfocata. NULL se non c'e' un
 * wallpaper o non si carica. La carichiamo gia' piccola (l'upscale morbido di
 * GtkPicture fa parte della sfocatura) e poi applichiamo qualche passata di
 * box blur per un risultato liscio a prescindere dal filtro della GPU. */
static GdkTexture *make_blurred_wallpaper(void)
{
    const char *path = pick_wallpaper_path();
    if (!path || !*path)
        return NULL;

    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 256, 256, TRUE, NULL);
    if (!pb)
        return NULL;

    int w  = gdk_pixbuf_get_width(pb);
    int h  = gdk_pixbuf_get_height(pb);
    int ch = gdk_pixbuf_get_n_channels(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    guchar *pix = gdk_pixbuf_get_pixels(pb);

    guchar *tmp = g_malloc((gsize) rs * h);
    for (int pass = 0; pass < 3; pass++) {       /* 3 passate ~ gaussiana */
        blur_pass(pix, tmp, w, h, ch, rs, 6, TRUE);
        blur_pass(tmp, pix, w, h, ch, rs, 6, FALSE);
    }
    g_free(tmp);

    GBytes *bytes = gdk_pixbuf_read_pixel_bytes(pb);   /* tiene vivo pb */
    GdkTexture *tex = gdk_memory_texture_new(w, h,
        (ch == 4) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, bytes, rs);
    g_bytes_unref(bytes);
    g_object_unref(pb);
    return tex;
}

/* Rigenera lo sfondo sfocato dal wallpaper attuale. Chiamata ad ogni apertura
 * cosi' segue eventuali cambi di wallpaper (hot-reload). Senza wallpaper la
 * picture resta vuota e trasparisce il velo scuro di fallback (CSS). */
static void launcher_refresh_blur(void)
{
    if (!l_blur_pic)
        return;
    GdkTexture *tex = make_blurred_wallpaper();
    gtk_picture_set_paintable(GTK_PICTURE(l_blur_pic),
                              tex ? GDK_PAINTABLE(tex) : NULL);
    if (tex)
        g_object_unref(tex);
}

/* ---- Costruzione popup -------------------------------------------------- */

static void build_popup(void)
{
    l_popup = gtk_window_new();
    gtk_widget_add_css_class(l_popup, "launcher-popup");

    gtk_layer_init_for_window(GTK_WINDOW(l_popup));
    /* TOP, sotto la barra (OVERLAY): il velo copre tutto lo schermo, barra
     * inclusa, ma la barra resta visibile sopra al velo. */
    gtk_layer_set_layer(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_LAYER_TOP);
    gtk_layer_set_namespace(GTK_WINDOW(l_popup), "sfshell-launcher");
    gtk_layer_set_anchor(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(l_popup), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    /* -1 = il velo ignora l'exclusive zone della barra e si estende ANCHE
     * dietro di essa (prima partiva sotto la barra, lasciando una striscia
     * non oscurata in alto). */
    gtk_layer_set_exclusive_zone(GTK_WINDOW(l_popup), -1);
    /* ON_DEMAND (non EXCLUSIVE): il launcher prende la tastiera quando e'
     * focato (per la ricerca), ma NON tiene un grab esclusivo. Con EXCLUSIVE
     * il compositor consumava il click sul tasto della barra come tentativo
     * di focus, per cui il secondo click nella stessa posizione non arrivava
     * mai al bottone e il launcher non si chiudeva. */
    gtk_layer_set_keyboard_mode(GTK_WINDOW(l_popup),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

    /* Backdrop a tutto schermo come stack (overlay): in fondo il wallpaper
     * sfocato (frosted glass, universale), sopra un velo scuro per staccare il
     * pannello, in cima il pannello centrale. Anima opacita' (fade). */
    GtkWidget *backdrop = gtk_overlay_new();
    l_backdrop = backdrop;
    gtk_widget_add_css_class(backdrop, "launcher-backdrop");
    gtk_widget_add_css_class(backdrop, "opening");
    gtk_window_set_child(GTK_WINDOW(l_popup), backdrop);

    /* Wallpaper sfocato (riempito, COVER) come sfondo: allineato al wallpaper
     * reale, sembra che il launcher lo smerigli. Non intercetta i click. */
    GtkWidget *blur = gtk_picture_new();
    l_blur_pic = blur;
    gtk_picture_set_content_fit(GTK_PICTURE(blur), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(blur), TRUE);
    gtk_widget_set_can_target(blur, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(backdrop), blur);

    /* Velo scuro sopra la sfocatura: profondita' + leggibilita'. */
    GtkWidget *tint = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(tint, "launcher-tint");
    gtk_widget_set_can_target(tint, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(backdrop), tint);

    /* Pannello centrale semitrasparente: lascia trasparire la sfocatura. */
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    l_panel = panel;
    gtk_widget_add_css_class(panel, "launcher-panel");
    gtk_widget_add_css_class(panel, "opening");
    gtk_widget_set_halign(panel, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(panel, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(backdrop), panel);

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
    l_scroll = scroll;
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
    gtk_flow_box_set_sort_func(GTK_FLOW_BOX(l_grid),
                               sort_cell, NULL, NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), l_grid);
    g_signal_connect(l_grid, "child-activated",
                     G_CALLBACK(on_child_activated), NULL);

    /* Rotellina -> scroll orizzontale. */
    GtkEventController *sc = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), scroll);
    gtk_widget_add_controller(scroll, sc);

    populate_grid();

    /* Escape chiude. Fase CAPTURE: il popup vede il tasto PRIMA del campo di
     * ricerca (che ha il focus e altrimenti "mangerebbe" Escape), cosi' ESC
     * chiude sempre il launcher a prescindere dal focus. */
    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
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
    search_reset();

    /* Rigenera lo sfondo sfocato dal wallpaper corrente (segue l'hot-reload). */
    launcher_refresh_blur();

    if (l_button)
        gtk_widget_add_css_class(l_button, "active");
    gtk_widget_add_css_class(l_backdrop, "opening");
    gtk_widget_add_css_class(l_panel, "opening");
    gtk_window_present(GTK_WINDOW(l_popup));
    gtk_widget_grab_focus(l_search);
    g_timeout_add_once(40, open_anim_cb, NULL);
}

void launcher_toggle(void)
{
    on_toggle(NULL, NULL);
}

static void test_open_once(gpointer btn)
{
    on_toggle(GTK_BUTTON(btn), NULL);
}

GtkWidget *launcher_button_new(void)
{
    GtkWidget *btn = gtk_button_new();
    l_button = btn;
    gtk_widget_add_css_class(btn, "launcher-btn");
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);

    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "launcher-dot");
    gtk_button_set_child(GTK_BUTTON(btn), dot);

    g_signal_connect(btn, "clicked", G_CALLBACK(on_toggle), NULL);

    /* Hook di debug: apre il launcher all'avvio se SFSHELL_LAUNCHER_TEST=1. */
    if (g_strcmp0(g_getenv("SFSHELL_LAUNCHER_TEST"), "1") == 0)
        g_timeout_add_once(600, test_open_once, btn);

    return btn;
}
