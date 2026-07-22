#include "colors.h"
#include "config.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ========================================================================
 *  Generazione palette dai wallpaper.
 *
 *  Pipeline:
 *   1) Raccoglie i wallpaper distinti in uso (wallpaper, wallpaper-extended,
 *      wallpaper-<CONNECTOR>).
 *   2) Per ognuno: downscale + median-cut -> N colori rappresentativi pesati.
 *   3) Se un solo wallpaper: quei colori sono il "seed pool". Se piu':
 *      cerca colori comuni a TUTTI (entro una soglia di distanza); se non ce
 *      ne sono, fonde i colori di tutti (unione pesata).
 *   4) Dal seed pool costruisce una palette sobria (accento poco saturo,
 *      neutri tinti leggermente dall'accento), dark o light secondo la
 *      luminosita' media (o color_scheme forzato).
 *   5) Scrive gtk-4.0/gtk.css, gtk-3.0/gtk.css e ~/.gtkrc-2.0 (overwrite,
 *      con backup una-tantum dei file non generati da noi).
 * ======================================================================== */

#define MARKER "sfshell generated palette"

/* ---- Colore e conversioni ----------------------------------------------- */

typedef struct { double r, g, b, w; } Rep;   /* r,g,b in 0..1, w = peso */

static double clamp01(double v) { return CLAMP(v, 0.0, 1.0); }

static void rgb_to_hsl(double r, double g, double b,
                       double *h, double *s, double *l)
{
    double mx = MAX(r, MAX(g, b));
    double mn = MIN(r, MIN(g, b));
    double d = mx - mn;
    *l = (mx + mn) / 2.0;
    if (d < 1e-9) { *h = 0; *s = 0; return; }
    *s = (*l > 0.5) ? d / (2.0 - mx - mn) : d / (mx + mn);
    double hh;
    if (mx == r)      hh = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (mx == g) hh = (b - r) / d + 2.0;
    else              hh = (r - g) / d + 4.0;
    *h = hh / 6.0;
}

static double hue2rgb(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

static void hsl_to_rgb(double h, double s, double l,
                       double *r, double *g, double *b)
{
    if (s < 1e-9) { *r = *g = *b = l; return; }
    double q = (l < 0.5) ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    *r = hue2rgb(p, q, h + 1.0 / 3.0);
    *g = hue2rgb(p, q, h);
    *b = hue2rgb(p, q, h - 1.0 / 3.0);
}

/* Luminanza percettiva (per contrasto/tono). */
static double luminance(double r, double g, double b)
{
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/* "#rrggbb" da HSL, in out[8]. */
static void hex_hsl(char *out, double h, double s, double l)
{
    double r, g, b;
    hsl_to_rgb(clamp01(h < 0 ? h + 1 : (h > 1 ? h - 1 : h)),
               clamp01(s), clamp01(l), &r, &g, &b);
    g_snprintf(out, 8, "#%02x%02x%02x",
               (int) lround(clamp01(r) * 255),
               (int) lround(clamp01(g) * 255),
               (int) lround(clamp01(b) * 255));
}

/* ---- Median cut --------------------------------------------------------- */

typedef struct { guint8 r, g, b; } Px;

static int g_cut_channel;   /* canale su cui ordinare (0=r,1=g,2=b) */

static int px_cmp(const void *a, const void *b)
{
    const Px *x = a, *y = b;
    int xv = (g_cut_channel == 0) ? x->r : (g_cut_channel == 1) ? x->g : x->b;
    int yv = (g_cut_channel == 0) ? y->r : (g_cut_channel == 1) ? y->g : y->b;
    return xv - yv;
}

typedef struct { int start, len; } Slice;

/* Range (max-min) di un canale in uno slice. */
static int slice_range(const Px *px, const Slice *s, int ch)
{
    int lo = 255, hi = 0;
    for (int i = s->start; i < s->start + s->len; i++) {
        int v = (ch == 0) ? px[i].r : (ch == 1) ? px[i].g : px[i].b;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    return hi - lo;
}

/* Median-cut su px[0..n): ritorna fino a K rappresentativi pesati. */
static Rep *median_cut(Px *px, int n, int K, int *out_count)
{
    if (n <= 0) { *out_count = 0; return NULL; }

    Slice *boxes = g_new(Slice, K);
    int nboxes = 1;
    boxes[0].start = 0;
    boxes[0].len = n;

    while (nboxes < K) {
        /* Box da tagliare: quello col range di canale piu' grande e len>1. */
        int best = -1, best_range = 0, best_ch = 0;
        for (int i = 0; i < nboxes; i++) {
            if (boxes[i].len < 2) continue;
            for (int ch = 0; ch < 3; ch++) {
                int r = slice_range(px, &boxes[i], ch);
                if (r > best_range) { best_range = r; best = i; best_ch = ch; }
            }
        }
        if (best < 0 || best_range == 0)
            break;   /* niente piu' da tagliare */

        Slice bx = boxes[best];
        g_cut_channel = best_ch;
        qsort(px + bx.start, bx.len, sizeof(Px), px_cmp);
        int half = bx.len / 2;

        boxes[best].len = half;
        boxes[nboxes].start = bx.start + half;
        boxes[nboxes].len = bx.len - half;
        nboxes++;
    }

    Rep *reps = g_new0(Rep, nboxes);
    for (int i = 0; i < nboxes; i++) {
        double sr = 0, sg = 0, sb = 0;
        Slice *s = &boxes[i];
        for (int j = s->start; j < s->start + s->len; j++) {
            sr += px[j].r; sg += px[j].g; sb += px[j].b;
        }
        reps[i].r = (sr / s->len) / 255.0;
        reps[i].g = (sg / s->len) / 255.0;
        reps[i].b = (sb / s->len) / 255.0;
        reps[i].w = (double) s->len / n;   /* normalizzato: sum = 1 */
    }
    g_free(boxes);
    *out_count = nboxes;
    return reps;
}

/* ---- Estrazione da un wallpaper ----------------------------------------- */

/* Rappresentativi di un'immagine + luminanza media. NULL se non caricabile. */
static Rep *extract_reps(const char *path, int K, int *count, double *mean_lum)
{
    *count = 0;
    *mean_lum = 0.5;
    GdkPixbuf *full = gdk_pixbuf_new_from_file(path, NULL);
    if (!full)
        return NULL;

    int w = gdk_pixbuf_get_width(full);
    int h = gdk_pixbuf_get_height(full);
    int maxdim = 160;
    double sc = (w > h) ? (double) maxdim / w : (double) maxdim / h;
    if (sc > 1) sc = 1;
    int nw = MAX(1, (int) (w * sc));
    int nh = MAX(1, (int) (h * sc));
    GdkPixbuf *small = gdk_pixbuf_scale_simple(full, nw, nh, GDK_INTERP_BILINEAR);
    g_object_unref(full);
    if (!small)
        return NULL;

    int nch = gdk_pixbuf_get_n_channels(small);
    int rs  = gdk_pixbuf_get_rowstride(small);
    const guint8 *pix = gdk_pixbuf_get_pixels(small);

    Px *arr = g_new(Px, (gsize) nw * nh);
    int m = 0;
    double lum = 0;
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            const guint8 *p = pix + y * rs + x * nch;
            if (nch == 4 && p[3] < 8)   /* salta i pixel quasi trasparenti */
                continue;
            arr[m].r = p[0]; arr[m].g = p[1]; arr[m].b = p[2];
            lum += luminance(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
            m++;
        }
    }
    g_object_unref(small);

    if (m == 0) { g_free(arr); return NULL; }
    *mean_lum = lum / m;

    Rep *reps = median_cut(arr, m, K, count);
    g_free(arr);
    return reps;
}

/* ---- Combinazione multi-wallpaper --------------------------------------- */

/* Distanza colore semplice (RGB euclidea, 0..~1.73). */
static double rep_dist(const Rep *a, const Rep *b)
{
    double dr = a->r - b->r, dg = a->g - b->g, db = a->b - b->b;
    return sqrt(dr * dr + dg * dg + db * db);
}

/* Cerca in reps il piu' vicino ad a entro thresh; ritorna indice o -1. */
static int nearest(const Rep *a, const Rep *reps, int n, double thresh)
{
    int best = -1;
    double bd = thresh;
    for (int i = 0; i < n; i++) {
        double d = rep_dist(a, &reps[i]);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Colori comuni a TUTTI i set (ancorati al primo). Ritorna array o NULL. */
static Rep *common_colors(Rep **sets, int *lens, int nsets,
                          double thresh, int *out_count)
{
    *out_count = 0;
    if (nsets < 2)
        return NULL;

    GArray *out = g_array_new(FALSE, FALSE, sizeof(Rep));
    for (int i = 0; i < lens[0]; i++) {
        Rep acc = sets[0][i];
        double sr = acc.r * acc.w, sg = acc.g * acc.w, sb = acc.b * acc.w;
        double sw = acc.w;
        gboolean in_all = TRUE;
        for (int j = 1; j < nsets; j++) {
            int k = nearest(&sets[0][i], sets[j], lens[j], thresh);
            if (k < 0) { in_all = FALSE; break; }
            Rep *m = &sets[j][k];
            sr += m->r * m->w; sg += m->g * m->w; sb += m->b * m->w;
            sw += m->w;
        }
        if (in_all && sw > 0) {
            Rep merged = { sr / sw, sg / sw, sb / sw, sw };
            g_array_append_val(out, merged);
        }
    }

    *out_count = out->len;
    if (out->len == 0) { g_array_free(out, TRUE); return NULL; }
    return (Rep *) g_array_free(out, FALSE);
}

/* Fusione: unione di tutti i rappresentativi (ognuno gia' pesato per set). */
static Rep *fuse_colors(Rep **sets, int *lens, int nsets, int *out_count)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(Rep));
    for (int j = 0; j < nsets; j++)
        for (int i = 0; i < lens[j]; i++) {
            Rep r = sets[j][i];
            r.w /= nsets;   /* ogni wallpaper contribuisce in egual misura */
            g_array_append_val(out, r);
        }
    *out_count = out->len;
    if (out->len == 0) { g_array_free(out, TRUE); return NULL; }
    return (Rep *) g_array_free(out, FALSE);
}

/* ---- Costruzione palette ------------------------------------------------- */

typedef struct {
    char accent[8], accent_fg[8];
    char bg[8], fg[8];
    char view_bg[8], view_fg[8];
    char card[8];
    char headerbar[8], headerbar_fg[8];
    char popover[8], popover_fg[8];
    char sidebar[8];
    char border[8];
    char selected_fg[8];
    char error[8], warning[8], success[8];
    gboolean dark;
} Palette;

static void build_palette(Rep *seed, int n, double mean_lum,
                          const char *scheme, Palette *pal)
{
    gboolean dark;
    if (g_strcmp0(scheme, "dark") == 0)       dark = TRUE;
    else if (g_strcmp0(scheme, "light") == 0) dark = FALSE;
    else                                      dark = (mean_lum < 0.5);
    pal->dark = dark;

    /* Tinta dominante: media CIRCOLARE dei toni pesata sulla cromaticita'
     * (w * s^2). Cosi' i grigi non spostano la tinta e non si creano colori
     * "ibridi" (es. rosso + grigio -> marrone) che nel wallpaper non esistono:
     * si ottiene il tono reale dominante (il rosso resta rosso).
     * Nel frattempo misuro:
     *  - mean_sat: saturazione media pesata (quanto e' colorato il wallpaper);
     *  - sa: saturazione tipica della sola parte cromatica (per l'accento). */
    double cx = 0, cy = 0, wchroma = 0;   /* accumulatori media circolare */
    double s_num = 0, s_den = 0;          /* saturazione cromatica pesata  */
    double sum_w = 0, sum_s = 0;          /* saturazione media pesata      */
    for (int i = 0; i < n; i++) {
        double h, s, l;
        rgb_to_hsl(seed[i].r, seed[i].g, seed[i].b, &h, &s, &l);
        double wi = seed[i].w;
        sum_s += s * wi;
        sum_w += wi;
        if (s > 0.12 && l > 0.08 && l < 0.95) {
            double cw = wi * s * s;       /* pesa forte i colori saturi */
            double ang = h * 2.0 * G_PI;
            cx += cw * cos(ang);
            cy += cw * sin(ang);
            wchroma += cw;
            s_num += wi * s * s;
            s_den += wi * s;
        }
    }
    double mean_sat = (sum_w > 0) ? sum_s / sum_w : 0.0;

    double ha, sa;
    if (wchroma > 0) {
        double ang = atan2(cy, cx);
        if (ang < 0) ang += 2.0 * G_PI;
        ha = ang / (2.0 * G_PI);
        sa = (s_den > 0) ? s_num / s_den : 0.40;
    } else {
        ha = 0.58; sa = 0.40;   /* fallback: blu sobrio (wallpaper acromatico) */
    }

    /* Fattore di tinta 0..1: quanto tingere di colore i neutri (sfondi, card,
     * ecc.). Un wallpaper grigio (mean_sat molto bassa) da' tint ~0 -> palette
     * neutra/nera; uno colorato da' tint ~1 -> superfici vivaci come prima.
     * La soglia scarta il rumore cromatico dei quasi-grigi. */
    double tint = CLAMP((mean_sat - 0.06) * 3.2, 0.0, 1.0);

    /* Accento: saturazione derivata da quella reale del colore scelto (nessun
     * minimo forzato), cosi' su un wallpaper spento l'accento resta tenue.
     * Luminosita' piuttosto chiara (in dark) per farlo spiccare. */
    double acc_l = dark ? 0.68 : 0.52;
    double acc_s = CLAMP(sa * 1.5, 0.0, dark ? 0.95 : 1.0);
    hex_hsl(pal->accent, ha, acc_s, acc_l);

    double ar, ag, ab;
    hsl_to_rgb(ha, acc_s, acc_l, &ar, &ag, &ab);
    const char *on_accent = (luminance(ar, ag, ab) > 0.55) ? "#1b1b1b"
                                                           : "#ffffff";
    g_strlcpy(pal->accent_fg, on_accent, sizeof pal->accent_fg);
    g_strlcpy(pal->selected_fg, on_accent, sizeof pal->selected_fg);

    /* Neutri tinti dalla tinta dominante (hue = ha). La saturazione base e'
     * alta (stile matugen: gli sfondi riflettono il wallpaper) ma viene
     * scalata per `tint`: su un wallpaper grigio i neutri restano quasi neri
     * neutri, su uno colorato risultano vivaci. */
    if (dark) {
        hex_hsl(pal->bg,           ha, 0.55 * tint, 0.135);
        hex_hsl(pal->view_bg,      ha, 0.48 * tint, 0.165);
        hex_hsl(pal->card,         ha, 0.46 * tint, 0.215);
        hex_hsl(pal->headerbar,    ha, 0.58 * tint, 0.15);
        hex_hsl(pal->popover,      ha, 0.46 * tint, 0.205);
        hex_hsl(pal->sidebar,      ha, 0.55 * tint, 0.15);
        hex_hsl(pal->border,       ha, 0.38 * tint, 0.32);
        hex_hsl(pal->fg,           ha, 0.28 * tint, 0.94);
        hex_hsl(pal->view_fg,      ha, 0.28 * tint, 0.94);
        hex_hsl(pal->headerbar_fg, ha, 0.28 * tint, 0.94);
        hex_hsl(pal->popover_fg,   ha, 0.28 * tint, 0.94);
    } else {
        hex_hsl(pal->bg,           ha, 0.52 * tint, 0.935);
        hex_hsl(pal->view_bg,      ha, 0.38 * tint, 0.985);
        hex_hsl(pal->card,         ha, 0.40 * tint, 0.965);
        hex_hsl(pal->headerbar,    ha, 0.55 * tint, 0.91);
        hex_hsl(pal->popover,      ha, 0.40 * tint, 0.965);
        hex_hsl(pal->sidebar,      ha, 0.48 * tint, 0.92);
        hex_hsl(pal->border,       ha, 0.40 * tint, 0.80);
        hex_hsl(pal->fg,           ha, 0.55 * tint, 0.14);
        hex_hsl(pal->view_fg,      ha, 0.55 * tint, 0.14);
        hex_hsl(pal->headerbar_fg, ha, 0.55 * tint, 0.14);
        hex_hsl(pal->popover_fg,   ha, 0.55 * tint, 0.14);
    }

    /* Semantici vividi, coerenti col tono. */
    double sl = dark ? 0.62 : 0.48;
    hex_hsl(pal->error,   0.00, 0.82, sl);   /* rosso   */
    hex_hsl(pal->warning, 0.11, 0.85, sl);   /* ambra   */
    hex_hsl(pal->success, 0.39, 0.68, sl);   /* verde   */
}

/* ---- Scrittura file GTK -------------------------------------------------- */

/* Scrive content in path (overwrite). Se esiste un file NON generato da noi,
 * ne salva una copia .sfshell-backup una sola volta. Crea le cartelle. */
static void write_managed(const char *path, const char *content)
{
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    char *old = NULL;
    if (g_file_get_contents(path, &old, NULL, NULL)) {
        gboolean ours = old && strstr(old, MARKER) != NULL;
        if (!ours) {
            char *bak = g_strconcat(path, ".sfshell-backup", NULL);
            if (!g_file_test(bak, G_FILE_TEST_EXISTS))
                g_file_set_contents(bak, old, -1, NULL);
            g_free(bak);
        }
    }
    g_free(old);

    g_file_set_contents(path, content, -1, NULL);
}

/* Blocco @define-color per GTK4: usato sia per il file sia per l'applicazione
 * a caldo alla shell. Stringa da liberare con g_free(). */
static char *build_gtk4_css(const Palette *p)
{
    char *css = g_strdup_printf(
        "/* %s - do not edit (rigenerato da sfshell) */\n"
        "@define-color accent_color %s;\n"
        "@define-color accent_bg_color %s;\n"
        "@define-color accent_fg_color %s;\n"
        "@define-color window_bg_color %s;\n"
        "@define-color window_fg_color %s;\n"
        "@define-color view_bg_color %s;\n"
        "@define-color view_fg_color %s;\n"
        "@define-color headerbar_bg_color %s;\n"
        "@define-color headerbar_fg_color %s;\n"
        "@define-color headerbar_border_color %s;\n"
        "@define-color headerbar_backdrop_color %s;\n"
        "@define-color card_bg_color %s;\n"
        "@define-color card_fg_color %s;\n"
        "@define-color popover_bg_color %s;\n"
        "@define-color popover_fg_color %s;\n"
        "@define-color dialog_bg_color %s;\n"
        "@define-color dialog_fg_color %s;\n"
        "@define-color sidebar_bg_color %s;\n"
        "@define-color sidebar_fg_color %s;\n"
        "@define-color destructive_color %s;\n"
        "@define-color destructive_bg_color %s;\n"
        "@define-color destructive_fg_color #ffffff;\n"
        "@define-color success_color %s;\n"
        "@define-color success_bg_color %s;\n"
        "@define-color success_fg_color #ffffff;\n"
        "@define-color warning_color %s;\n"
        "@define-color warning_bg_color %s;\n"
        "@define-color error_color %s;\n"
        "@define-color error_bg_color %s;\n"
        "@define-color error_fg_color #ffffff;\n"
        "@define-color shade_color %s;\n"
        "@define-color scrollbar_outline_color %s;\n"
        "/* nomi legacy per app non-adwaita */\n"
        "@define-color theme_bg_color %s;\n"
        "@define-color theme_fg_color %s;\n"
        "@define-color theme_base_color %s;\n"
        "@define-color theme_text_color %s;\n"
        "@define-color theme_selected_bg_color %s;\n"
        "@define-color theme_selected_fg_color %s;\n"
        "@define-color theme_unfocused_bg_color %s;\n"
        "@define-color theme_unfocused_fg_color %s;\n"
        "@define-color insensitive_bg_color %s;\n"
        "@define-color insensitive_fg_color %s;\n"
        "@define-color borders %s;\n"
        "@define-color unfocused_borders %s;\n",
        MARKER,
        p->accent, p->accent, p->accent_fg,
        p->bg, p->fg, p->view_bg, p->view_fg,
        p->headerbar, p->headerbar_fg, p->border, p->bg,
        p->card, p->fg, p->popover, p->popover_fg,
        p->card, p->fg, p->sidebar, p->fg,
        p->error, p->error,
        p->success, p->success, p->warning, p->warning, p->error, p->error,
        p->border, p->border,
        p->bg, p->fg, p->view_bg, p->view_fg,
        p->accent, p->selected_fg, p->bg, p->fg,
        p->card, p->border, p->border, p->border);

    /* Named color libadwaita restanti, cosi' e' coperta OGNI superficie. */
    char *extra = g_strdup_printf(
        "@define-color sidebar_backdrop_color %s;\n"
        "@define-color sidebar_shade_color %s;\n"
        "@define-color secondary_sidebar_bg_color %s;\n"
        "@define-color secondary_sidebar_fg_color %s;\n"
        "@define-color secondary_sidebar_backdrop_color %s;\n"
        "@define-color secondary_sidebar_shade_color %s;\n"
        "@define-color headerbar_shade_color %s;\n"
        "@define-color card_shade_color %s;\n"
        "@define-color popover_shade_color %s;\n"
        "@define-color dialog_backdrop_color %s;\n"
        "@define-color thumbnail_bg_color %s;\n"
        "@define-color thumbnail_fg_color %s;\n"
        "@define-color window_radius 12px;\n",
        p->bg, p->border, p->popover, p->fg, p->bg, p->border,
        p->border, p->border, p->border, p->bg, p->card, p->fg);

    char *out = g_strconcat(css, extra, NULL);
    g_free(css);
    g_free(extra);
    return out;
}

static void emit_gtk3(const Palette *p)
{
    char *css = g_strdup_printf(
        "/* %s - do not edit (rigenerato da sfshell) */\n"
        "@define-color theme_bg_color %s;\n"
        "@define-color theme_fg_color %s;\n"
        "@define-color theme_base_color %s;\n"
        "@define-color theme_text_color %s;\n"
        "@define-color theme_selected_bg_color %s;\n"
        "@define-color theme_selected_fg_color %s;\n"
        "@define-color insensitive_bg_color %s;\n"
        "@define-color insensitive_fg_color %s;\n"
        "@define-color borders %s;\n"
        "@define-color unfocused_borders %s;\n"
        "@define-color warning_color %s;\n"
        "@define-color error_color %s;\n"
        "@define-color success_color %s;\n"
        "@define-color accent_color %s;\n"
        "@define-color accent_bg_color %s;\n"
        "@define-color accent_fg_color %s;\n",
        MARKER,
        p->bg, p->fg, p->view_bg, p->view_fg,
        p->accent, p->selected_fg, p->card, p->fg,
        p->border, p->border,
        p->warning, p->error, p->success,
        p->accent, p->accent, p->accent_fg);

    char *path = g_build_filename(g_get_user_config_dir(),
                                  "gtk-3.0", "gtk.css", NULL);
    write_managed(path, css);
    g_free(path);
    g_free(css);
}

static void emit_gtk2(const Palette *p)
{
    char *rc = g_strdup_printf(
        "# %s - do not edit (rigenerato da sfshell)\n"
        "gtk-color-scheme = \"bg_color:%s\\nfg_color:%s\\n"
        "base_color:%s\\ntext_color:%s\\n"
        "selected_bg_color:%s\\nselected_fg_color:%s\\n"
        "tooltip_bg_color:%s\\ntooltip_fg_color:%s\"\n",
        MARKER,
        p->bg, p->fg, p->view_bg, p->view_fg,
        p->accent, p->selected_fg, p->card, p->fg);

    char *path = g_build_filename(g_get_home_dir(), ".gtkrc-2.0", NULL);
    write_managed(path, rc);
    g_free(path);
    g_free(rc);
}

/* ---- Orchestrazione ----------------------------------------------------- */

/* Percorsi dei wallpaper distinti in uso (verificati esistenti). GStrv. */
static char **collect_wallpapers(void)
{
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray  *out  = g_ptr_array_new();

    const char *singles[] = { "wallpaper", "wallpaper-extended" };
    for (guint i = 0; i < G_N_ELEMENTS(singles); i++) {
        const char *p = config_get(singles[i]);
        if (p && *p && !g_hash_table_contains(seen, p) &&
            g_file_test(p, G_FILE_TEST_EXISTS)) {
            g_hash_table_add(seen, (gpointer) p);
            g_ptr_array_add(out, g_strdup(p));
        }
    }

    char **keys = config_keys_with_prefix("wallpaper-");
    for (int i = 0; keys && keys[i]; i++) {
        if (g_strcmp0(keys[i], "wallpaper-extended") == 0)
            continue;
        const char *p = config_get(keys[i]);
        if (p && *p && !g_hash_table_contains(seen, p) &&
            g_file_test(p, G_FILE_TEST_EXISTS)) {
            g_hash_table_add(seen, (gpointer) p);
            g_ptr_array_add(out, g_strdup(p));
        }
    }
    g_strfreev(keys);

    g_hash_table_destroy(seen);
    g_ptr_array_add(out, NULL);
    return (char **) g_ptr_array_free(out, FALSE);
}

/* Firma per l'hot-reload: evita di rigenerare se nulla e' cambiato. */
static char *g_last_sig = NULL;

/* Provider CSS ad alta priorita' sul display: applica la palette A CALDO alla
 * shell in esecuzione (ridefinisce i colori del tema usati dalla barra). */
static GtkCssProvider *g_live = NULL;

static void apply_live(const char *css)
{
    if (g_live)
        gtk_css_provider_load_from_string(g_live, css);
}

static char *build_signature(char **paths, const char *scheme)
{
    GString *s = g_string_new(scheme ? scheme : "auto");
    for (int i = 0; paths && paths[i]; i++) {
        GStatBuf st;
        gint64 mtime = (g_stat(paths[i], &st) == 0) ? (gint64) st.st_mtime : 0;
        g_string_append_printf(s, "|%s:%" G_GINT64_FORMAT, paths[i], mtime);
    }
    return g_string_free(s, FALSE);
}

static void generate(void)
{
    if (g_strcmp0(config_get("generate_colors"), "true") != 0) {
        /* Disabilitato: rimuove l'override a caldo (torna al tema di default)
         * e azzera la firma cosi' una riattivazione rigenera. */
        apply_live("");
        g_free(g_last_sig);
        g_last_sig = NULL;
        return;
    }

    char **paths = collect_wallpapers();
    int nsets = g_strv_length(paths);
    if (nsets == 0) { g_strfreev(paths); return; }

    const char *scheme = config_get("color_scheme");

    /* Skip se identico all'ultima generazione (path + mtime + scheme). */
    char *sig = build_signature(paths, scheme);
    if (g_last_sig && g_strcmp0(sig, g_last_sig) == 0) {
        g_free(sig);
        g_strfreev(paths);
        return;
    }

    const int K = 12;
    Rep  **sets = g_new0(Rep *, nsets);
    int   *lens = g_new0(int, nsets);
    double lum_sum = 0;
    int    valid = 0;
    for (int i = 0; i < nsets; i++) {
        double ml;
        sets[i] = extract_reps(paths[i], K, &lens[i], &ml);
        if (sets[i]) { lum_sum += ml; valid++; }
    }

    if (valid == 0) {
        for (int i = 0; i < nsets; i++) g_free(sets[i]);
        g_free(sets); g_free(lens); g_free(sig); g_strfreev(paths);
        return;
    }

    /* Compatta i set validi. */
    Rep **vsets = g_new0(Rep *, valid);
    int  *vlens = g_new0(int, valid);
    int   vi = 0;
    for (int i = 0; i < nsets; i++)
        if (sets[i]) { vsets[vi] = sets[i]; vlens[vi] = lens[i]; vi++; }

    /* Seed pool: singolo -> i suoi colori; multipli -> comuni o fusione. */
    Rep *seed = NULL;
    int  seed_n = 0;
    gboolean seed_owned = FALSE;   /* seed va liberato a parte? */
    if (valid == 1) {
        seed = vsets[0];
        seed_n = vlens[0];
    } else {
        seed = common_colors(vsets, vlens, valid, 0.16, &seed_n);
        if (seed) {
            seed_owned = TRUE;
        } else {
            seed = fuse_colors(vsets, vlens, valid, &seed_n);
            seed_owned = TRUE;
        }
    }

    if (seed && seed_n > 0) {
        Palette pal;
        build_palette(seed, seed_n, lum_sum / valid, scheme, &pal);

        /* GTK4: stessa CSS per il file su disco e per l'applicazione a caldo. */
        char *g4 = build_gtk4_css(&pal);
        char *p4 = g_build_filename(g_get_user_config_dir(),
                                    "gtk-4.0", "gtk.css", NULL);
        write_managed(p4, g4);
        apply_live(g4);           /* la barra si ricolora subito */
        g_free(p4);
        g_free(g4);

        emit_gtk3(&pal);
        emit_gtk2(&pal);

        g_free(g_last_sig);
        g_last_sig = sig;
        sig = NULL;

        g_message("sfshell: palette %s generata da %d wallpaper",
                  pal.dark ? "dark" : "light", valid);
    }

    if (seed_owned) g_free(seed);
    for (int i = 0; i < nsets; i++) g_free(sets[i]);
    g_free(vsets); g_free(vlens);
    g_free(sets); g_free(lens);
    g_free(sig);
    g_strfreev(paths);
}

void colors_init(void)
{
    /* Provider a caldo, priorita' sopra a tutti gli altri della shell cosi'
     * i colori della palette vincono sui @define-color del tema. */
    GdkDisplay *dpy = gdk_display_get_default();
    if (dpy) {
        g_live = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            dpy, GTK_STYLE_PROVIDER(g_live),
            GTK_STYLE_PROVIDER_PRIORITY_USER + 3);
    }
    generate();
}

void colors_reload(void)
{
    generate();
}
