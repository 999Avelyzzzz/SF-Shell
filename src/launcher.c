#include "launcher.h"
#include "fuzzy.h"
#include "config.h"
#include "usage.h"
#include <gtk4-layer-shell.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

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
#define CELL_H    82    /* icona 56 + spaziatura + label: niente vuoto sotto */

/* Tetto ai risultati mostrati durante una ricerca (primi N per punteggio). */
#define LAUNCHER_MAX_RESULTS 8

/* Rilevanza: si mostrano solo i risultati "veri". Soglia assoluta (sotto =
 * rumore, non un match) e finestra relativa al miglior punteggio (mostra le
 * alternative vicine al migliore, es. chrome/chromium). Valori tarabili. */
#define LAUNCHER_MIN_SCORE 0.30
#define LAUNCHER_REL_GAP   0.40

/* Dati precalcolati per cella: nomi in minuscolo (per il match fuzzy), ultimo
 * punteggio calcolato e statistiche d'uso (per l'ordinamento della griglia). */
typedef struct {
    char  *name_lc;
    char  *id_lc;
    double score;
    int    use_count;   /* lanci totali (0 = mai usata)     */
    gint64 use_last;    /* ultimo uso (epoch, secondi)      */
} Cell;

/* Chiave stabile di un'app per la cache d'uso: l'id del .desktop, con fallback
 * al nome visualizzato. Puntatore di proprieta' del GAppInfo (non liberare). */
static const char *app_key(GAppInfo *app)
{
    const char *id = g_app_info_get_id(app);
    if (id && *id)
        return id;
    return g_app_info_get_display_name(app);
}

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
static GtkWidget *l_scrollbar; /* scrollbar orizzontale disegnata da noi   */
static GPtrArray *l_buttons;  /* tasti nelle barre (uno per monitor)     */

/* Scrollbar custom: pillola arrotondata su una traccia sottile, che segue
 * l'hadjustment della griglia. Hover -> cresce/schiarisce (animato, smooth);
 * si puo' trascinare per scorrere. Sostituisce quella di GTK (policy EXTERNAL). */
#define SB_MARGIN     10.0   /* rientro orizzontale traccia (px)          */
#define SB_MIN_THUMB  36.0   /* larghezza minima del cursore (px)         */
#define SB_THICK      4.0    /* spessore a riposo (px)                    */
#define SB_THICK_MAX  7.0    /* spessore in hover/drag (px)               */

static double   l_sb_hover;        /* stato hover animato 0..1 (thumb)    */
static gboolean l_sb_hover_target; /* hover cursore: verso dell'animazione*/
static double   l_sb_show;         /* dissolvenza 0..1 (mouse sul launcher)*/
static gboolean l_sb_show_target;  /* show: verso dell'animazione         */
static guint    l_sb_anim;         /* tick callback delle animazioni      */
static gboolean l_sb_inside;       /* puntatore sopra la scrollbar        */
static gboolean l_over_launcher;   /* puntatore sopra il pannello launcher*/
static gboolean l_sb_dragging;     /* trascinamento in corso              */
static double   l_sb_grab_offset;  /* offset presa dentro il cursore (px) */
static char      *l_query;    /* testo di ricerca (lowercase)            */
static guint      l_close_timer; /* timer del fade-out di chiusura        */

/* Cache dei GtkFlowBoxChild in ordine d'inserimento: iterarla e' O(n) senza
 * il costo per-elemento di gtk_flow_box_get_child_at_index (che scandisce la
 * lista dei figli, rendendo la ricerca live O(n^2)). */
static GPtrArray *l_cells;

/* Celle "riempitivo" invisibili: completano l'ultima pagina 6x4 cosi' la
 * disposizione a colonne del FlowBox si legge per righe (impaginazione stile
 * Launchpad). Sono figli del FlowBox ma senza icona ne' interazione. */
static GPtrArray *l_fillers;

/* Monitoraggio installazioni app: quando cambiano i .desktop la griglia va
 * rigenerata. l_dirty = rebuild rimandato alla prossima apertura; il timer
 * coalizza i cambi ravvicinati (installazioni che toccano piu' file). */
static GAppInfoMonitor *l_app_monitor;
static gboolean         l_dirty;
static guint            l_rebuild_timer;

static void rebuild_grid(void);
static void on_search_changed(GtkSearchEntry *entry, gpointer u);
static void sb_sync(void);
static void relayout_pages(void);

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

/* Aggiunge/toglie lo stato ".active" al pallino di TUTTE le barre. */
static void buttons_set_active(gboolean active)
{
    if (!l_buttons)
        return;
    for (guint i = 0; i < l_buttons->len; i++) {
        GtkWidget *b = l_buttons->pdata[i];
        if (active)
            gtk_widget_add_css_class(b, "active");
        else
            gtk_widget_remove_css_class(b, "active");
    }
}

/* Chiude con animazione: velo in fade-out, pannello che rimpicciolisce dal
 * centro; la finestra viene nascosta a transizione finita. */
static void launcher_hide(void)
{
    if (!l_popup || !gtk_widget_get_visible(l_popup) || l_close_timer)
        return;
    buttons_set_active(FALSE);
    gtk_widget_add_css_class(l_backdrop, "closing");
    gtk_widget_add_css_class(l_panel, "closing");
    l_close_timer = g_timeout_add_once(LAUNCHER_CLOSE_MS,
                                       launcher_do_hide, NULL);
}

/* Child-setup eseguito nel processo figlio tra fork ed exec: setsid() lo
 * stacca in una NUOVA sessione (nuovo process group, senza terminale di
 * controllo). Cosi' l'app non condivide piu' il gruppo con la shell: quando
 * la shell viene chiusa/uccisa il figlio viene riadottato da init e NON
 * riceve i segnali diretti al gruppo della shell -> resta viva. Equivale a
 * un "disown". Solo funzioni async-signal-safe qui: setsid() lo e'. */
static void launch_child_setup(gpointer u G_GNUC_UNUSED)
{
    setsid();
}

/* Riallinea le statistiche d'uso di ogni cella dalla cache e ri-ordina la
 * griglia. Chiamata dopo un lancio: al prossimo (o immediato) refresh l'app
 * appena usata sale nell'ordine. */
static void refresh_usage_order(void)
{
    if (!l_cells)
        return;
    for (guint i = 0; i < l_cells->len; i++) {
        GtkFlowBoxChild *ch = l_cells->pdata[i];
        Cell *c = g_object_get_data(G_OBJECT(ch), "cell");
        GAppInfo *app = g_object_get_data(G_OBJECT(ch), "app");
        if (c && app)
            usage_get(app_key(app), &c->use_count, &c->use_last);
    }
    if (!l_searching)
        relayout_pages();       /* riassegna le posizioni di pagina */
}

static void launch_app(GAppInfo *app)
{
    if (!app)
        return;
    GdkDisplay *display = gdk_display_get_default();
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(display);
    GError *err = NULL;

    /* I .desktop sono GDesktopAppInfo: li lanciamo "as manager" per poter
     * imporre il child-setup (setsid) e detachare stdout/stderr, restando
     * fedeli alla semantica del .desktop (Exec, Terminal=, DBusActivatable).
     * Fallback al lancio generico per gli app-info non-desktop. */
    if (G_IS_DESKTOP_APP_INFO(app)) {
        g_desktop_app_info_launch_uris_as_manager(
            G_DESKTOP_APP_INFO(app), NULL, G_APP_LAUNCH_CONTEXT(ctx),
            G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                G_SPAWN_STDERR_TO_DEV_NULL,
            launch_child_setup, NULL, NULL, NULL, &err);
    } else {
        g_app_info_launch(app, NULL, G_APP_LAUNCH_CONTEXT(ctx), &err);
    }
    gboolean ok = (err == NULL);
    if (err) {
        g_warning("launcher: lancio fallito: %s", err->message);
        g_error_free(err);
    }

    /* Solo i lanci riusciti contano per l'ordinamento d'uso. */
    if (ok) {
        usage_bump(app_key(app));
        refresh_usage_order();
    }

    g_object_unref(ctx);
    launcher_hide();
}

static void on_child_activated(GtkFlowBox *box G_GNUC_UNUSED,
                               GtkFlowBoxChild *child, gpointer u G_GNUC_UNUSED)
{
    launch_app(g_object_get_data(G_OBJECT(child), "app"));
}

/* ---- Ricerca / impaginazione -------------------------------------------- */

static gboolean is_filler(GtkFlowBoxChild *child)
{
    return g_object_get_data(G_OBJECT(child), "filler") != NULL;
}

static int grid_pos(GtkFlowBoxChild *child)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "gridpos"));
}

/* Visibilita' di una cella:
 *  - riempitivi: visibili SOLO nella griglia (tengono lo slot di pagina),
 *    nascosti in ricerca;
 *  - app: in ricerca solo se il punteggio rientra tra i migliori (>= cutoff);
 *    senza pattern il cutoff e' negativo, quindi si vedono tutte. */
static gboolean filter_cell(GtkFlowBoxChild *child, gpointer u G_GNUC_UNUSED)
{
    if (is_filler(child))
        return !l_searching;
    Cell *c = g_object_get_data(G_OBJECT(child), "cell");
    return c && c->score >= l_cutoff;
}

/* Confronto per l'ordine LOGICO (usato per assegnare le posizioni di pagina):
 * per uso (piu' lanciate prima, poi le piu' recenti), a parita' alfabetico.
 * Senza dati d'uso (default) e' puro A-Z. */
static int logical_cmp(gconstpointer a, gconstpointer b)
{
    GtkFlowBoxChild *ca_ch = *(GtkFlowBoxChild *const *) a;
    GtkFlowBoxChild *cb_ch = *(GtkFlowBoxChild *const *) b;
    Cell *ca = g_object_get_data(G_OBJECT(ca_ch), "cell");
    Cell *cb = g_object_get_data(G_OBJECT(cb_ch), "cell");
    if (!ca || !cb)
        return 0;
    if (ca->use_count != cb->use_count)
        return cb->use_count - ca->use_count;
    if (ca->use_last != cb->use_last)
        return (cb->use_last > ca->use_last) ? 1 : -1;
    return g_utf8_collate(ca->name_lc, cb->name_lc);
}

/* Ordina il FlowBox:
 *  - in ricerca: per punteggio fuzzy decrescente (i riempitivi in coda);
 *  - nella griglia: per posizione di pagina precalcolata (relayout_pages),
 *    che rende la disposizione a colonne leggibile per righe. */
static int sort_cell(GtkFlowBoxChild *a, GtkFlowBoxChild *b,
                     gpointer u G_GNUC_UNUSED)
{
    if (!l_searching)
        return grid_pos(a) - grid_pos(b);

    gboolean fa = is_filler(a), fb = is_filler(b);
    if (fa || fb)
        return fa - fb;                            /* riempitivi sempre in coda */

    Cell *ca = g_object_get_data(G_OBJECT(a), "cell");
    Cell *cb = g_object_get_data(G_OBJECT(b), "cell");
    if (!ca || !cb)
        return 0;
    if (cb->score > ca->score) return 1;
    if (cb->score < ca->score) return -1;
    return g_utf8_collate(ca->name_lc, cb->name_lc);
}

/* Assegna a ogni cella la posizione nel FlowBox (colonne da GRID_ROWS) in modo
 * che, pagina per pagina (GRID_COLS x GRID_ROWS), l'ordine logico si legga per
 * RIGHE. I riempitivi occupano gli slot finali dell'ultima pagina. */
static void relayout_pages(void)
{
    if (!l_cells || !l_grid)
        return;

    const int R = GRID_ROWS, C = GRID_COLS, PAGE = R * C;

    /* Ordine logico delle app. */
    GPtrArray *ord = g_ptr_array_new();
    for (guint i = 0; i < l_cells->len; i++)
        g_ptr_array_add(ord, l_cells->pdata[i]);
    g_ptr_array_sort(ord, logical_cmp);

    /* i-esima app in ordine -> slot (colonna*R + riga) per lettura a righe. */
    guint total = ord->len + (l_fillers ? l_fillers->len : 0);
    for (guint i = 0; i < total; i++) {
        int p = (int) (i / PAGE);
        int o = (int) (i % PAGE);
        int row = o / C, col = o % C;
        int pos = (p * C + col) * R + row;
        GtkFlowBoxChild *ch = (i < ord->len)
            ? ord->pdata[i]
            : l_fillers->pdata[i - ord->len];
        g_object_set_data(G_OBJECT(ch), "gridpos", GINT_TO_POINTER(pos));
    }
    g_ptr_array_free(ord, TRUE);

    if (!l_searching)
        gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(l_grid));
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
        /* EXTERNAL: scrollabile ma senza scrollbar di GTK (la nostra e' custom). */
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(l_scroll),
                                       GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
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
    sb_sync();
}

/* Azzera lo stato di ricerca: tutte le celle visibili, impaginazione a righe. */
static void search_reset(void)
{
    l_searching = FALSE;
    l_cutoff = -1.0;
    l_best = NULL;
    for (guint i = 0; l_cells && i < l_cells->len; i++) {
        Cell *c = g_object_get_data(G_OBJECT(l_cells->pdata[i]), "cell");
        if (c) c->score = 0.0;
    }
    apply_result_layout(0);
    relayout_pages();           /* riordino per pagine (usa l'uso aggiornato) */
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(l_grid));
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

    guint n = l_cells ? l_cells->len : 0;

    /* Calcola il punteggio di ogni cella e raccogli i valori per la soglia.
     * Buffer punteggi su stack finche' le app stanno in STACK_SCORES, cosi'
     * la ricerca live non alloca a ogni tasto. */
    enum { STACK_SCORES = 512 };
    double stack_scores[STACK_SCORES];
    double *scores = (n <= STACK_SCORES) ? stack_scores
                                         : g_new(double, n);
    double best_score = -1.0;
    guint ns = 0;
    for (guint i = 0; i < n; i++) {
        GtkFlowBoxChild *ch = l_cells->pdata[i];
        Cell *c = g_object_get_data(G_OBJECT(ch), "cell");
        if (!c) continue;
        c->score = cell_match(c, pat);
        scores[ns++] = c->score;
        if (c->score > best_score) {
            best_score = c->score;
            l_best = ch;
        }
    }

    /* Punteggio che delimita i primi N (tetto massimo di risultati). */
    double eighth = 0.0;
    if (ns > 0) {
        qsort(scores, ns, sizeof(double), cmp_double_desc);
        guint k = MIN((guint) LAUNCHER_MAX_RESULTS, ns);
        eighth = scores[k - 1];
    }
    if (scores != stack_scores)
        g_free(scores);
    g_free(pat);

    /* Soglia finale = max(soglia assoluta, migliore - finestra, tetto top-N).
     * Se nemmeno il migliore raggiunge la soglia assoluta, non passa nulla. */
    l_cutoff = LAUNCHER_MIN_SCORE;
    double rel = best_score - LAUNCHER_REL_GAP;
    if (rel > l_cutoff)    l_cutoff = rel;
    if (eighth > l_cutoff) l_cutoff = eighth;

    /* Conta i risultati che passano, per centrarli con la forma giusta. */
    int nvis = 0;
    for (guint i = 0; i < n; i++) {
        Cell *c = g_object_get_data(G_OBJECT(l_cells->pdata[i]), "cell");
        if (c && c->score >= l_cutoff) nvis++;
    }

    apply_result_layout(nvis);
    gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(l_grid));
    gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(l_grid));
}

/* Invio nella ricerca: lancia il miglior risultato (o la prima cella visibile). */
static void on_search_activate(GtkSearchEntry *entry,
                               gpointer u G_GNUC_UNUSED)
{
    /* Invio "al volo": se si digita velocissimo e si preme subito, il
     * search-changed puo' non aver ancora ricalcolato (scatta sul main loop).
     * Se il testo corrente non combacia con l'ultima query elaborata,
     * riallineiamo QUI, in modo sincrono, cosi' l_best riflette esattamente
     * cio' che c'e' scritto e Invio lancia il match giusto (dolphin), non la
     * prima cella della griglia. */
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    char *now = g_utf8_strdown(txt ? txt : "", -1);
    if (g_strcmp0(now, l_query ? l_query : "") != 0)
        on_search_changed(entry, NULL);
    g_free(now);

    /* In ricerca: lancia il miglior match, ma solo se supera davvero la soglia
     * (altrimenti "nessun risultato" -> non aprire nulla a caso). Il controllo
     * e' sul PUNTEGGIO, non su child_visible: quest'ultimo si aggiorna dopo il
     * re-filtro del FlowBox (asincrono) e sarebbe ancora vecchio qui. */
    if (l_searching) {
        if (l_best) {
            Cell *cb = g_object_get_data(G_OBJECT(l_best), "cell");
            if (cb && cb->score >= l_cutoff)
                launch_app(g_object_get_data(G_OBJECT(l_best), "app"));
        }
        return;
    }

    /* Senza ricerca (query vuota): apri la prima app della griglia. */
    if (l_cells && l_cells->len > 0)
        launch_app(g_object_get_data(G_OBJECT(l_cells->pdata[0]), "app"));
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

/* Cella "riempitivo": stesso ingombro di una app ma vuota e non cliccabile.
 * Serve solo a completare l'ultima pagina 6x4 per l'impaginazione a righe. */
static void add_filler(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, CELL_W, CELL_H);
    gtk_widget_set_can_target(box, FALSE);
    gtk_flow_box_append(GTK_FLOW_BOX(l_grid), box);

    GtkFlowBoxChild *child = GTK_FLOW_BOX_CHILD(gtk_widget_get_parent(box));
    gtk_widget_set_can_target(GTK_WIDGET(child), FALSE);
    g_object_set_data(G_OBJECT(child), "filler", GINT_TO_POINTER(1));
    g_ptr_array_add(l_fillers, child);
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
        usage_get(app_key(app), &c->use_count, &c->use_last);
        g_object_set_data_full(G_OBJECT(child), "cell", c, cell_free);

        g_ptr_array_add(l_cells, child);
    }

    g_list_free_full(apps, g_object_unref);

    /* Completa l'ultima pagina con riempitivi, cosi' il rimappaggio a righe e'
     * esatto anche quando le app non sono un multiplo di GRID_COLS*GRID_ROWS. */
    int page = GRID_COLS * GRID_ROWS;
    int pad = (page - ((int) l_cells->len % page)) % page;
    for (int i = 0; i < pad; i++)
        add_filler();

    relayout_pages();
}

/* Svuota la griglia (e la cache) prima di ripopolarla. */
static void clear_grid(void)
{
    GtkWidget *ch;
    while ((ch = gtk_widget_get_first_child(l_grid)))
        gtk_flow_box_remove(GTK_FLOW_BOX(l_grid), ch);
    if (l_cells)
        g_ptr_array_set_size(l_cells, 0);
    if (l_fillers)
        g_ptr_array_set_size(l_fillers, 0);
}

/* Rigenera la griglia dai .desktop attuali, poi riapplica lo stato di ricerca
 * corrente (se l'utente sta gia' cercando, la lista risultati resta coerente). */
static void rebuild_grid(void)
{
    clear_grid();
    populate_grid();
    l_dirty = FALSE;

    if (l_searching && l_query && *l_query)
        on_search_changed(GTK_SEARCH_ENTRY(l_search), NULL);
    else
        search_reset();
}

static void rebuild_timeout(gpointer u G_GNUC_UNUSED)
{
    l_rebuild_timer = 0;
    rebuild_grid();
}

/* I .desktop installati sono cambiati (nuova app, rimozione, aggiornamento).
 * Se il launcher e' aperto rigeneriamo subito (con debounce per coalizzare i
 * cambi ravvicinati); se e' chiuso segniamo solo "dirty" e rigenereremo alla
 * prossima apertura, senza sprecare lavoro a schermo spento. */
static void on_apps_changed(GAppInfoMonitor *m G_GNUC_UNUSED,
                            gpointer u G_GNUC_UNUSED)
{
    l_dirty = TRUE;
    if (l_popup && gtk_widget_get_visible(l_popup)) {
        if (l_rebuild_timer)
            g_source_remove(l_rebuild_timer);
        l_rebuild_timer = g_timeout_add_once(300, rebuild_timeout, NULL);
    }
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

/* Scroll fluido della rotellina: ogni notch aggiorna un valore-obiettivo e un
 * tick sul frame-clock avvicina dolcemente la posizione reale (nessun salto). */
#define WHEEL_STEP     130.0   /* px per notch di rotellina                   */
#define WHEEL_EASE     0.22    /* frazione di avvicinamento per frame         */

static double   l_wheel_target;   /* posizione orizzontale desiderata (px)   */
static gboolean l_wheel_active;   /* animazione wheel in corso               */
static guint    l_wheel_tick;     /* tick callback dello scroll fluido       */

static GtkAdjustment *scroll_hadj(void)
{
    return l_scroll
        ? gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(l_scroll))
        : NULL;
}

/* Ferma lo scroll fluido (es. quando si trascina la scrollbar a mano). */
static void wheel_stop(void)
{
    l_wheel_active = FALSE;
    if (l_wheel_tick && l_scroll) {
        gtk_widget_remove_tick_callback(l_scroll, l_wheel_tick);
        l_wheel_tick = 0;
    }
}

static gboolean wheel_tick(GtkWidget *w G_GNUC_UNUSED,
                           GdkFrameClock *clock G_GNUC_UNUSED,
                           gpointer u G_GNUC_UNUSED)
{
    GtkAdjustment *h = scroll_hadj();
    if (!h) {
        l_wheel_tick = 0;
        l_wheel_active = FALSE;
        return G_SOURCE_REMOVE;
    }
    double cur = gtk_adjustment_get_value(h);
    double d = l_wheel_target - cur;
    if (fabs(d) < 0.5) {
        gtk_adjustment_set_value(h, l_wheel_target);
        l_wheel_tick = 0;
        l_wheel_active = FALSE;
        return G_SOURCE_REMOVE;
    }
    gtk_adjustment_set_value(h, cur + d * WHEEL_EASE);
    return G_SOURCE_CONTINUE;
}

/* Rotellina verticale -> scroll orizzontale fluido (le app vanno verso destra). */
static gboolean on_scroll(GtkEventControllerScroll *c G_GNUC_UNUSED,
                          double dx, double dy, gpointer scroll)
{
    GtkAdjustment *h =
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll));
    if (!h)
        return FALSE;
    double delta = (dy != 0.0) ? dy : dx;
    if (delta == 0.0)
        return FALSE;

    /* Accumula sul target (partendo dalla posizione reale se fermo), clampato. */
    double lower = gtk_adjustment_get_lower(h);
    double maxv  = gtk_adjustment_get_upper(h) - gtk_adjustment_get_page_size(h);
    if (!l_wheel_active)
        l_wheel_target = gtk_adjustment_get_value(h);
    l_wheel_target += delta * WHEEL_STEP;
    if (l_wheel_target < lower) l_wheel_target = lower;
    if (l_wheel_target > maxv)  l_wheel_target = maxv;
    l_wheel_active = TRUE;

    if (!l_wheel_tick)
        l_wheel_tick = gtk_widget_add_tick_callback(l_scroll, wheel_tick,
                                                    NULL, NULL);
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

/* Cache dello sfondo sfocato: la sfocatura (lettura da disco + decode + 6
 * passate di box blur) e' costosa e va fatta solo quando il wallpaper cambia
 * davvero. Memorizziamo path + mtime dell'ultimo blur: se combaciano con quelli
 * correnti, riusiamo la texture gia' impostata sulla picture. Cosi' l'apertura
 * del launcher e' immediata a wallpaper invariato (il caso normale). */
static char    *l_blur_path;   /* path del wallpaper sfocato in cache        */
static gint64   l_blur_mtime;  /* mtime del file al momento del blur          */

/* Rigenera lo sfondo sfocato dal wallpaper attuale. Chiamata ad ogni apertura
 * cosi' segue eventuali cambi di wallpaper (hot-reload). Senza wallpaper la
 * picture resta vuota e trasparisce il velo scuro di fallback (CSS). */
static void launcher_refresh_blur(void)
{
    if (!l_blur_pic)
        return;

    const char *path = pick_wallpaper_path();

    /* mtime corrente del wallpaper (0 se non c'e' o non si legge). */
    gint64 mtime = 0;
    if (path && *path) {
        GStatBuf st;
        if (g_stat(path, &st) == 0)
            mtime = (gint64) st.st_mtime;
    }

    /* Stesso file, stesso mtime, texture gia' presente: niente da rifare. */
    if (g_strcmp0(path, l_blur_path) == 0 && mtime == l_blur_mtime &&
        gtk_picture_get_paintable(GTK_PICTURE(l_blur_pic)) != NULL)
        return;

    GdkTexture *tex = make_blurred_wallpaper();
    gtk_picture_set_paintable(GTK_PICTURE(l_blur_pic),
                              tex ? GDK_PAINTABLE(tex) : NULL);
    if (tex)
        g_object_unref(tex);

    /* Aggiorna la chiave di cache (anche in caso di fallimento: evita di
     * riprovare a vuoto un wallpaper illeggibile ad ogni apertura). */
    g_free(l_blur_path);
    l_blur_path  = g_strdup(path);
    l_blur_mtime = mtime;
}

/* ---- Scrollbar orizzontale custom --------------------------------------- */

/* Geometria del cursore per una data larghezza del widget. active=FALSE se non
 * c'e' overflow (niente da scorrere). */
typedef struct {
    double track_x, track_w;   /* traccia                                  */
    double thumb_x, thumb_w;   /* cursore (thumb_x assoluto)               */
    double max_x;              /* corsa massima del cursore                */
    gboolean active;
} SbGeom;

static GtkAdjustment *sb_hadj(void)
{
    if (!l_scroll)
        return NULL;
    return gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(l_scroll));
}

static SbGeom sb_geom(int width)
{
    SbGeom g = { 0 };
    GtkAdjustment *h = sb_hadj();
    if (!h || width <= 0)
        return g;

    double lower = gtk_adjustment_get_lower(h);
    double upper = gtk_adjustment_get_upper(h);
    double page  = gtk_adjustment_get_page_size(h);
    double value = gtk_adjustment_get_value(h);
    double span  = upper - lower;
    if (span <= page + 0.5 || span <= 0.0)
        return g;                       /* nessun overflow */

    g.track_x = SB_MARGIN;
    g.track_w = width - 2.0 * SB_MARGIN;
    if (g.track_w <= 0.0)
        return g;

    double thumb_w = g.track_w * (page / span);
    if (thumb_w < SB_MIN_THUMB) thumb_w = SB_MIN_THUMB;
    if (thumb_w > g.track_w)    thumb_w = g.track_w;
    g.thumb_w = thumb_w;
    g.max_x = g.track_w - thumb_w;

    double denom = span - page;
    double frac = denom > 0.0 ? (value - lower) / denom : 0.0;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    g.thumb_x = g.track_x + frac * g.max_x;
    g.active = TRUE;
    return g;
}

/* Percorso di un rettangolo arrotondato (pillola). */
static void sb_rrect(cairo_t *cr, double x, double y, double w, double h,
                     double r)
{
    if (r > h / 2.0) r = h / 2.0;
    if (r > w / 2.0) r = w / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0,         G_PI / 2.0);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2.0,  G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,        G_PI * 1.5);
    cairo_close_path(cr);
}

static void sb_draw(GtkDrawingArea *area, cairo_t *cr,
                    int width, int height, gpointer u G_GNUC_UNUSED)
{
    SbGeom g = sb_geom(width);
    if (!g.active)
        return;

    double vis = l_sb_show;                       /* dissolvenza complessiva */
    if (vis <= 0.003)
        return;                                   /* mouse fuori: invisibile */

    GdkRGBA c;
    gtk_widget_get_color(GTK_WIDGET(area), &c);

    double t = l_sb_hover;                        /* 0..1 */
    double thick = SB_THICK + (SB_THICK_MAX - SB_THICK) * t;
    double y = (height - thick) / 2.0;
    double r = thick / 2.0;

    /* Traccia: sempre sottile e discreta. */
    sb_rrect(cr, g.track_x, y, g.track_w, thick, r);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, (0.10 + 0.05 * t) * vis);
    cairo_fill(cr);

    /* Cursore: piu' presente, si accende in hover/drag. */
    double a = 0.38 + 0.42 * t;
    if (l_sb_dragging)
        a = 0.92;
    sb_rrect(cr, g.thumb_x, y, g.thumb_w, thick, r);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, a * vis);
    cairo_fill(cr);
}

/* Mostra/nasconde la scrollbar in base all'overflow (indipendente dalla
 * larghezza gia' allocata) e ridisegna. */
static void sb_sync(void)
{
    if (!l_scrollbar)
        return;
    GtkAdjustment *h = sb_hadj();
    gboolean show = FALSE;
    if (h) {
        double span = gtk_adjustment_get_upper(h) - gtk_adjustment_get_lower(h);
        show = span > gtk_adjustment_get_page_size(h) + 0.5;
    }
    gtk_widget_set_visible(l_scrollbar, show);
    gtk_widget_queue_draw(l_scrollbar);
}

static void sb_on_adjustment(void)
{
    sb_sync();
}

/* ---- Scrollbar: animazioni (hover del cursore + dissolvenza) ------------- */

/* Avvicina `*val` a `target` di una frazione `k`; TRUE finche' non e' fermo. */
static gboolean sb_ease(double *val, gboolean target, double k)
{
    double t = target ? 1.0 : 0.0;
    double d = t - *val;
    if (fabs(d) < 0.01) {
        *val = t;
        return FALSE;
    }
    *val += d * k;
    return TRUE;
}

static gboolean sb_anim_cb(GtkWidget *w, GdkFrameClock *clock G_GNUC_UNUSED,
                           gpointer u G_GNUC_UNUSED)
{
    gboolean running = FALSE;
    running |= sb_ease(&l_sb_hover, l_sb_hover_target, 0.22);
    running |= sb_ease(&l_sb_show,  l_sb_show_target,  0.18);
    gtk_widget_queue_draw(w);
    if (!running) {
        l_sb_anim = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void sb_kick_anim(void)
{
    if (!l_sb_anim && l_scrollbar)
        l_sb_anim = gtk_widget_add_tick_callback(l_scrollbar, sb_anim_cb,
                                                 NULL, NULL);
}

static void sb_set_hover(gboolean on)
{
    l_sb_hover_target = on;
    sb_kick_anim();
}

static void sb_set_show(gboolean on)
{
    l_sb_show_target = on;
    sb_kick_anim();
}

/* Puntatore sopra/fuori la scrollbar stessa: solo l'ingrossamento del cursore. */
static void sb_on_enter(GtkEventControllerMotion *c G_GNUC_UNUSED,
                        double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                        gpointer u G_GNUC_UNUSED)
{
    l_sb_inside = TRUE;
    sb_set_hover(TRUE);
}

static void sb_on_leave(GtkEventControllerMotion *c G_GNUC_UNUSED,
                        gpointer u G_GNUC_UNUSED)
{
    l_sb_inside = FALSE;
    if (!l_sb_dragging)
        sb_set_hover(FALSE);
}

/* Puntatore sopra/fuori il pannello del launcher: mostra/nasconde la scrollbar. */
static void sb_on_launcher_enter(GtkEventControllerMotion *c G_GNUC_UNUSED,
                                 double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                                 gpointer u G_GNUC_UNUSED)
{
    l_over_launcher = TRUE;
    sb_set_show(TRUE);
}

static void sb_on_launcher_leave(GtkEventControllerMotion *c G_GNUC_UNUSED,
                                 gpointer u G_GNUC_UNUSED)
{
    l_over_launcher = FALSE;
    if (!l_sb_dragging)
        sb_set_show(FALSE);
}

/* ---- Scrollbar: trascinamento ------------------------------------------- */

/* Porta il bordo sinistro del cursore a `thumb_left` (assoluto), aggiornando
 * l'adjustment. */
static void sb_apply_thumb_left(double thumb_left, SbGeom g)
{
    if (!g.active || g.max_x <= 0.0)
        return;
    double lo = g.track_x, hi = g.track_x + g.max_x;
    if (thumb_left < lo) thumb_left = lo;
    if (thumb_left > hi) thumb_left = hi;

    GtkAdjustment *h = sb_hadj();
    if (!h)
        return;
    double lower = gtk_adjustment_get_lower(h);
    double upper = gtk_adjustment_get_upper(h);
    double page  = gtk_adjustment_get_page_size(h);
    double frac  = (thumb_left - g.track_x) / g.max_x;
    gtk_adjustment_set_value(h, lower + frac * ((upper - lower) - page));
}

static void sb_drag_begin(GtkGestureDrag *gesture G_GNUC_UNUSED,
                          double start_x, double start_y G_GNUC_UNUSED,
                          gpointer u G_GNUC_UNUSED)
{
    SbGeom g = sb_geom(gtk_widget_get_width(l_scrollbar));
    if (!g.active)
        return;
    wheel_stop();               /* la mano vince sull'inerzia della rotellina */
    l_sb_dragging = TRUE;
    sb_set_hover(TRUE);

    if (start_x >= g.thumb_x && start_x <= g.thumb_x + g.thumb_w) {
        /* Presa sul cursore: trascinamento relativo. */
        l_sb_grab_offset = start_x - g.thumb_x;
    } else {
        /* Presa sulla traccia: il cursore si centra sotto il puntatore. */
        l_sb_grab_offset = g.thumb_w / 2.0;
        sb_apply_thumb_left(start_x - l_sb_grab_offset, g);
    }
    gtk_widget_queue_draw(l_scrollbar);
}

static void sb_drag_update(GtkGestureDrag *gesture, double off_x,
                           double off_y G_GNUC_UNUSED, gpointer u G_GNUC_UNUSED)
{
    double start_x = 0.0;
    gtk_gesture_drag_get_start_point(gesture, &start_x, NULL);
    SbGeom g = sb_geom(gtk_widget_get_width(l_scrollbar));
    if (!g.active)
        return;
    sb_apply_thumb_left((start_x + off_x) - l_sb_grab_offset, g);
}

static void sb_drag_end(GtkGestureDrag *gesture G_GNUC_UNUSED,
                        double off_x G_GNUC_UNUSED, double off_y G_GNUC_UNUSED,
                        gpointer u G_GNUC_UNUSED)
{
    l_sb_dragging = FALSE;
    sb_set_hover(l_sb_inside);
    sb_set_show(l_over_launcher || l_sb_inside);
    gtk_widget_queue_draw(l_scrollbar);
}

/* Costruisce il widget scrollbar e lo collega all'hadjustment della griglia. */
static GtkWidget *build_scrollbar(void)
{
    GtkWidget *sb = gtk_drawing_area_new();
    l_scrollbar = sb;
    gtk_widget_add_css_class(sb, "launcher-scrollbar");
    gtk_widget_set_hexpand(sb, TRUE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(sb), 14);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(sb), sb_draw, NULL, NULL);
    gtk_widget_set_visible(sb, FALSE);

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(sb_on_enter), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(sb_on_leave), NULL);
    gtk_widget_add_controller(sb, motion);

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(sb_drag_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(sb_drag_update), NULL);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(sb_drag_end), NULL);
    gtk_widget_add_controller(sb, GTK_EVENT_CONTROLLER(drag));

    GtkAdjustment *h = sb_hadj();
    if (h) {
        g_signal_connect(h, "changed",
                         G_CALLBACK(sb_on_adjustment), NULL);
        g_signal_connect(h, "value-changed",
                         G_CALLBACK(sb_on_adjustment), NULL);
    }
    return sb;
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

    /* La scrollbar custom compare solo col puntatore sopra il launcher. */
    GtkEventController *pm = gtk_event_controller_motion_new();
    g_signal_connect(pm, "enter", G_CALLBACK(sb_on_launcher_enter), NULL);
    g_signal_connect(pm, "leave", G_CALLBACK(sb_on_launcher_leave), NULL);
    gtk_widget_add_controller(panel, pm);

    /* Barra di ricerca. */
    l_search = gtk_search_entry_new();
    gtk_widget_add_css_class(l_search, "launcher-search");
    gtk_widget_set_hexpand(l_search, TRUE);
    /* Nessun debounce: "search-changed" scatta a OGNI lettera, cosi' la
     * griglia si aggiorna istantaneamente mentre si digita (di default
     * GtkSearchEntry aspetta ~150ms, dando la sensazione di lag). */
    gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(l_search), 0);
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
    /* EXTERNAL: nessuna scrollbar di GTK; la disegniamo noi (build_scrollbar)
     * mantenendo comunque attivo lo scorrimento dell'adjustment. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
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

    /* Scrollbar orizzontale disegnata da noi, sotto la griglia. */
    gtk_box_append(GTK_BOX(panel), build_scrollbar());

    usage_init();               /* cache d'uso caricata prima di ordinare */
    l_cells = g_ptr_array_new();
    l_fillers = g_ptr_array_new();
    populate_grid();

    /* Segue le installazioni/rimozioni di app a runtime: la griglia si
     * aggiorna senza riavviare la shell. */
    l_app_monitor = g_app_info_monitor_get();
    g_signal_connect(l_app_monitor, "changed",
                     G_CALLBACK(on_apps_changed), NULL);

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

    /* La scrollbar riparte nascosta: si mostrera' quando il puntatore entra
     * nel pannello (enter del motion controller). */
    l_over_launcher = FALSE;
    l_sb_show = 0.0;
    l_sb_show_target = FALSE;
    l_sb_hover = 0.0;
    l_sb_hover_target = FALSE;
    wheel_stop();               /* nessuna inerzia residua dall'apertura prima */

    /* App cambiate mentre il launcher era chiuso: rigenera ora, cosi' si apre
     * gia' aggiornato. (rebuild_grid azzera anche lo stato di ricerca.) */
    if (l_dirty)
        rebuild_grid();
    else
        search_reset();

    /* Rigenera lo sfondo sfocato dal wallpaper corrente (segue l'hot-reload). */
    launcher_refresh_blur();

    buttons_set_active(TRUE);
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

/* Un tasto distrutto (barra chiusa): toglilo dalla lista. */
static void on_button_destroy(gpointer data, GObject *where)
{
    if (l_buttons)
        g_ptr_array_remove_fast(l_buttons, where);
    (void) data;
}

GtkWidget *launcher_button_new(void)
{
    GtkWidget *btn = gtk_button_new();
    if (!l_buttons)
        l_buttons = g_ptr_array_new();
    g_ptr_array_add(l_buttons, btn);
    g_object_weak_ref(G_OBJECT(btn), on_button_destroy, NULL);
    gtk_widget_add_css_class(btn, "launcher-btn");
    gtk_widget_set_focusable(btn, FALSE);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);

    /* Se il launcher e' gia' aperto (barra aggiunta a caldo), riflettilo. */
    if (l_popup && gtk_widget_get_visible(l_popup) && !l_close_timer)
        gtk_widget_add_css_class(btn, "active");

    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "launcher-dot");
    gtk_button_set_child(GTK_BUTTON(btn), dot);

    g_signal_connect(btn, "clicked", G_CALLBACK(on_toggle), NULL);

    /* Hook di debug: apre il launcher all'avvio se SFSHELL_LAUNCHER_TEST=1. */
    if (g_strcmp0(g_getenv("SFSHELL_LAUNCHER_TEST"), "1") == 0)
        g_timeout_add_once(600, test_open_once, btn);

    return btn;
}
