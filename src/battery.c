#include "battery.h"

#include <glib.h>
#include <math.h>
#include <stdlib.h>

/* ========================================================================
 *  Batteria in stile macOS (Tahoe): "37%" a sinistra, poi il pittogramma
 *  della batteria. Passando il mouse compare una bolla (pill) semitrasparente
 *  dietro numero e glifo; al click la pill diventa bianca piena, testo/glifo
 *  neri, e si apre un popover in stile macOS con percentuale, glifo grande,
 *  stato, condizione della batteria, autonomia, cicli e alimentazione.
 *
 *  Numero e glifo della barra sono elementi distinti: il numero e' una
 *  GtkLabel (stesso font della barra), il glifo e' disegnato in cairo.
 *
 *  Sorgente dati: /sys/class/power_supply/<BATx>. Polling ogni 5s. Nessuna
 *  batteria di sistema (desktop) -> widget nascosto, niente spazio in barra.
 * ===================================================================== */

#define BAT_POLL_SEC   5
#define BAT_LOW_PCT    20    /* soglia "scarica": riempimento rosso           */

static int      b_capacity = -1;      /* -1 = nessuna batteria                */
static gboolean b_charging;           /* status == Charging                   */
static char    *b_dir;                /* /sys/class/power_supply/<BATx>       */

/* Un'istanza per barra (una barra per monitor): teniamo i widget da aggiornare
 * ad ogni tick e i campi del popover. */
typedef struct {
    GtkWidget *button;        /* GtkToggleButton (pill: hover + checked)      */
    GtkWidget *label;         /* "37%" nella barra                            */
    GtkWidget *glyph;         /* GtkDrawingArea del pittogramma in barra      */
    GtkWidget *popover;       /* menu macOS-like                              */
    /* Campi dinamici del popover. */
    GtkWidget *pop_glyph;
    GtkWidget *pop_pct;
    GtkWidget *pop_status;
    GtkWidget *pop_cond_row,   *pop_cond_val;
    GtkWidget *pop_time_row,   *pop_time_key, *pop_time_val;
    GtkWidget *pop_cycles_row, *pop_cycles_val;
    GtkWidget *pop_source_val;
} Battery;

/* ---- Lettura da sysfs --------------------------------------------------- */

static gboolean is_system_battery(const char *base, const char *name)
{
    char *type_path = g_build_filename(base, name, "type", NULL);
    char *type = NULL;
    gboolean is_bat = FALSE;
    if (g_file_get_contents(type_path, &type, NULL, NULL)) {
        g_strstrip(type);
        is_bat = (g_strcmp0(type, "Battery") == 0);
    }
    g_free(type);
    g_free(type_path);
    if (!is_bat)
        return FALSE;

    /* scope=Device -> periferica (mouse/tastiera): scarta. */
    char *scope_path = g_build_filename(base, name, "scope", NULL);
    char *scope = NULL;
    gboolean is_device = FALSE;
    if (g_file_get_contents(scope_path, &scope, NULL, NULL)) {
        g_strstrip(scope);
        is_device = (g_strcmp0(scope, "Device") == 0);
    }
    g_free(scope);
    g_free(scope_path);
    if (is_device)
        return FALSE;

    char *cap = g_build_filename(base, name, "capacity", NULL);
    gboolean has_cap = g_file_test(cap, G_FILE_TEST_EXISTS);
    g_free(cap);
    return has_cap;
}

/* Trova la batteria di sistema (preferisce "BAT*"). Path da g_free o NULL. */
static char *battery_find_dir(void)
{
    const char *base = "/sys/class/power_supply";
    GDir *dir = g_dir_open(base, 0, NULL);
    if (!dir)
        return NULL;

    char *found = NULL;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (!is_system_battery(base, name))
            continue;
        if (g_str_has_prefix(name, "BAT")) {
            g_free(found);
            found = g_build_filename(base, name, NULL);
            break;
        }
        if (!found)
            found = g_build_filename(base, name, NULL);
    }
    g_dir_close(dir);
    return found;
}

/* Intero "piccolo" da un file sysfs (percentuali, cicli). -1 se assente. */
static int read_int_file(const char *dir, const char *leaf)
{
    char *path = g_build_filename(dir, leaf, NULL);
    char *contents = NULL;
    int val = -1;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        val = atoi(contents);
        g_free(contents);
    }
    g_free(path);
    return val;
}

/* Intero a 64 bit (energie/cariche in µWh/µAh). -1 se assente. */
static gint64 read_int64_file(const char *dir, const char *leaf)
{
    char *path = g_build_filename(dir, leaf, NULL);
    char *contents = NULL;
    gint64 val = -1;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        val = g_ascii_strtoll(contents, NULL, 10);
        g_free(contents);
    }
    g_free(path);
    return val;
}

/* Stringa (status, ecc.) senza newline, da g_free. NULL se assente. */
static char *read_str_file(const char *dir, const char *leaf)
{
    char *path = g_build_filename(dir, leaf, NULL);
    char *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL))
        g_strstrip(contents);
    g_free(path);
    return contents;
}

/* Aggiorna capacity + stato di carica (per barra e glifo). */
static void battery_read(void)
{
    if (!b_dir) { b_capacity = -1; return; }

    int cap = read_int_file(b_dir, "capacity");
    if (cap < 0) { b_capacity = -1; return; }
    b_capacity = CLAMP(cap, 0, 100);

    b_charging = FALSE;
    char *status = read_str_file(b_dir, "status");
    if (status) {
        b_charging = (g_strcmp0(status, "Charging") == 0);
        g_free(status);
    }
}

/* Salute della batteria in %: energy_full/energy_full_design (o charge_*).
 * -1 se non disponibile. */
static int battery_health_pct(void)
{
    if (!b_dir) return -1;
    gint64 full   = read_int64_file(b_dir, "energy_full");
    gint64 design = read_int64_file(b_dir, "energy_full_design");
    if (full < 0 || design <= 0) {
        full   = read_int64_file(b_dir, "charge_full");
        design = read_int64_file(b_dir, "charge_full_design");
    }
    if (full < 0 || design <= 0) return -1;
    return CLAMP((int) ((full * 100) / design), 0, 100);
}

/* Ore rimanenti (allo scarico o alla carica). FALSE se non calcolabile. */
static gboolean battery_time_hours(double *out, gboolean charging)
{
    if (!b_dir) return FALSE;
    gint64 now  = read_int64_file(b_dir, "energy_now");
    gint64 rate = read_int64_file(b_dir, "power_now");
    gint64 full = read_int64_file(b_dir, "energy_full");
    if (now < 0 || rate <= 0) {
        now  = read_int64_file(b_dir, "charge_now");
        rate = read_int64_file(b_dir, "current_now");
        full = read_int64_file(b_dir, "charge_full");
    }
    if (now < 0 || rate <= 0) return FALSE;

    gint64 rem = charging ? ((full >= 0 ? full : now) - now) : now;
    if (rem < 0) rem = 0;
    *out = (double) rem / (double) rate;    /* µWh/µW = h, µAh/µA = h */
    return TRUE;
}

/* ---- Disegno del glifo (scala con la dimensione del widget) -------------- */

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    if (r < 0.0)     r = 0.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0,         G_PI / 2.0);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2.0,  G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,        G_PI * 1.5);
    cairo_close_path(cr);
}

static void glyph_draw(GtkDrawingArea *area, cairo_t *cr,
                       int width, int height, gpointer u G_GNUC_UNUSED)
{
    if (b_capacity < 0)
        return;

    /* Colore del glifo: "color" CSS del widget. In barra diventa nero quando
     * la pill e' attiva (checked); nel popover resta sempre chiaro. */
    GdkRGBA fg;
    gtk_widget_get_color(GTK_WIDGET(area), &fg);

    /* Geometria proporzionale all'altezza: stesso disegno per barra e popover. */
    double H = height;
    double lw    = MAX(1.0, H * 0.09);
    double bh    = H * 0.62;
    double bw    = bh * 2.15;
    double radius = bh * 0.30;
    double inset = MAX(1.4, bh * 0.17);
    double nub_h = bh * 0.42;
    double nub_w = MAX(1.4, bh * 0.16);
    double gap   = bh * 0.10;
    double total = bw + gap + nub_w;
    double x0 = floor((width  - total) / 2.0) + 0.5;  if (x0 < 0.5) x0 = 0.5;
    double y0 = floor((height - bh)    / 2.0) + 0.5;  if (y0 < 0.5) y0 = 0.5;

    /* Contorno del corpo. */
    rrect(cr, x0, y0, bw, bh, radius);
    cairo_set_line_width(cr, lw);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.90);
    cairo_stroke(cr);

    /* Polo positivo a destra. */
    double nx = x0 + bw + gap;
    double ny = y0 + (bh - nub_h) / 2.0;
    rrect(cr, nx, ny, nub_w, nub_h, nub_w * 0.45);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.90);
    cairo_fill(cr);

    /* Riempimento: rosso se scarica, verde in carica, altrimenti colore glifo. */
    GdkRGBA fill = fg;
    if (b_charging)                     { fill.red = 0.26; fill.green = 0.80; fill.blue = 0.36; }
    else if (b_capacity <= BAT_LOW_PCT) { fill.red = 0.95; fill.green = 0.27; fill.blue = 0.23; }

    double ix = x0 + inset, iy = y0 + inset;
    double iw = bw - 2.0 * inset, ih = bh - 2.0 * inset;
    double fw = iw * (b_capacity / 100.0);
    if (fw > 0.5) {
        cairo_save(cr);
        rrect(cr, ix, iy, iw, ih, radius - inset);
        cairo_clip(cr);
        cairo_rectangle(cr, ix, iy, fw, ih);
        cairo_set_source_rgba(cr, fill.red, fill.green, fill.blue, 1.0);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

/* ---- Popover: costruzione e aggiornamento ------------------------------- */

/* Riga "chiave a sinistra / valore a destra" in stile menu macOS. */
static GtkWidget *make_row(const char *key_text,
                           GtkWidget **key_out, GtkWidget **val_out)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "battery-menu-row");

    GtkWidget *k = gtk_label_new(key_text);
    gtk_widget_add_css_class(k, "battery-menu-key");
    gtk_widget_set_halign(k, GTK_ALIGN_START);
    gtk_widget_set_hexpand(k, TRUE);

    GtkWidget *v = gtk_label_new(NULL);
    gtk_widget_add_css_class(v, "battery-menu-val");
    gtk_widget_set_halign(v, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(row), k);
    gtk_box_append(GTK_BOX(row), v);
    if (key_out) *key_out = k;
    if (val_out) *val_out = v;
    return row;
}

static void set_cond_class(GtkWidget *v, const char *cls)
{
    gtk_widget_remove_css_class(v, "battery-cond-good");
    gtk_widget_remove_css_class(v, "battery-cond-fair");
    gtk_widget_remove_css_class(v, "battery-cond-poor");
    gtk_widget_add_css_class(v, cls);
}

static void battery_refresh_popover(Battery *b)
{
    if (b_capacity < 0)
        return;

    char t[24];
    g_snprintf(t, sizeof t, "%d%%", b_capacity);
    gtk_label_set_text(GTK_LABEL(b->pop_pct), t);
    gtk_widget_queue_draw(b->pop_glyph);

    /* Stato + fonte di alimentazione. */
    char *status = read_str_file(b_dir, "status");
    const char *status_txt = "Alimentazione a batteria";
    const char *source_txt = "Batteria";
    gboolean charging = FALSE, full = FALSE;
    if (status) {
        if (g_strcmp0(status, "Charging") == 0) {
            status_txt = "In carica"; source_txt = "Alimentatore"; charging = TRUE;
        } else if (g_strcmp0(status, "Full") == 0) {
            status_txt = "Carica completa"; source_txt = "Alimentatore"; full = TRUE;
        } else if (g_strcmp0(status, "Not charging") == 0) {
            status_txt = "Non in carica"; source_txt = "Alimentatore";
        }
    }
    gtk_label_set_text(GTK_LABEL(b->pop_status), status_txt);
    gtk_label_set_text(GTK_LABEL(b->pop_source_val), source_txt);

    /* Condizione (salute). */
    int health = battery_health_pct();
    if (health < 0) {
        gtk_widget_set_visible(b->pop_cond_row, FALSE);
    } else {
        const char *cond, *cls;
        if (health >= 80)      { cond = "Buona";        cls = "battery-cond-good"; }
        else if (health >= 60) { cond = "Media";        cls = "battery-cond-fair"; }
        else                   { cond = "Da sostituire"; cls = "battery-cond-poor"; }
        char cv[48];
        g_snprintf(cv, sizeof cv, "%s \xC2\xB7 %d%%", cond, health);   /* "Buona · 95%" */
        gtk_label_set_text(GTK_LABEL(b->pop_cond_val), cv);
        set_cond_class(b->pop_cond_val, cls);
        gtk_widget_set_visible(b->pop_cond_row, TRUE);
    }

    /* Autonomia / tempo alla carica. */
    double hours;
    if (!full && battery_time_hours(&hours, charging)) {
        int H = (int) hours;
        int M = (int) lround((hours - H) * 60.0);
        if (M >= 60) { H++; M -= 60; }
        char tv[16];
        g_snprintf(tv, sizeof tv, "%d:%02d", H, M);
        gtk_label_set_text(GTK_LABEL(b->pop_time_val), tv);
        gtk_label_set_text(GTK_LABEL(b->pop_time_key),
                           charging ? "Tempo alla carica" : "Autonomia");
        gtk_widget_set_visible(b->pop_time_row, TRUE);
    } else {
        gtk_widget_set_visible(b->pop_time_row, FALSE);
    }

    /* Cicli di carica. */
    int cycles = read_int_file(b_dir, "cycle_count");
    if (cycles >= 0) {
        char cy[16];
        g_snprintf(cy, sizeof cy, "%d", cycles);
        gtk_label_set_text(GTK_LABEL(b->pop_cycles_val), cy);
        gtk_widget_set_visible(b->pop_cycles_row, TRUE);
    } else {
        gtk_widget_set_visible(b->pop_cycles_row, FALSE);
    }

    g_free(status);
}

static void battery_build_popover(Battery *b)
{
    b->popover = gtk_popover_new();
    gtk_widget_add_css_class(b->popover, "battery-menu");
    gtk_popover_set_has_arrow(GTK_POPOVER(b->popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(b->popover), GTK_POS_BOTTOM);
    gtk_widget_set_parent(b->popover, b->button);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(content, 250, -1);
    gtk_popover_set_child(GTK_POPOVER(b->popover), content);

    /* Testata: glifo grande + percentuale + stato. */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    b->pop_glyph = gtk_drawing_area_new();
    gtk_widget_add_css_class(b->pop_glyph, "battery-pop-glyph");
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->pop_glyph), 54);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(b->pop_glyph), 26);
    gtk_widget_set_valign(b->pop_glyph, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(b->pop_glyph),
                                   glyph_draw, NULL, NULL);
    gtk_box_append(GTK_BOX(head), b->pop_glyph);

    GtkWidget *head_txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(head_txt, GTK_ALIGN_CENTER);
    b->pop_pct = gtk_label_new(NULL);
    gtk_widget_add_css_class(b->pop_pct, "battery-menu-pct");
    gtk_widget_set_halign(b->pop_pct, GTK_ALIGN_START);
    b->pop_status = gtk_label_new(NULL);
    gtk_widget_add_css_class(b->pop_status, "battery-menu-status");
    gtk_widget_set_halign(b->pop_status, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(head_txt), b->pop_pct);
    gtk_box_append(GTK_BOX(head_txt), b->pop_status);
    gtk_box_append(GTK_BOX(head), head_txt);
    gtk_box_append(GTK_BOX(content), head);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(sep, "battery-menu-sep");
    gtk_box_append(GTK_BOX(content), sep);

    /* Righe informative. */
    GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    b->pop_cond_row = make_row("Condizione", NULL, &b->pop_cond_val);
    gtk_box_append(GTK_BOX(rows), b->pop_cond_row);

    b->pop_time_row = make_row("Autonomia", &b->pop_time_key, &b->pop_time_val);
    gtk_box_append(GTK_BOX(rows), b->pop_time_row);

    b->pop_cycles_row = make_row("Cicli", NULL, &b->pop_cycles_val);
    gtk_box_append(GTK_BOX(rows), b->pop_cycles_row);

    GtkWidget *src_row = make_row("Alimentazione", NULL, &b->pop_source_val);
    gtk_box_append(GTK_BOX(rows), src_row);

    gtk_box_append(GTK_BOX(content), rows);
}

/* ---- Interazione: hover pill, click, popover ---------------------------- */

static void on_pop_closed(GtkPopover *p G_GNUC_UNUSED, gpointer data)
{
    Battery *b = data;
    /* Chiusura (click fuori/Esc): sincronizza la pill (torna non-attiva). */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->button), FALSE);
}

static void on_toggled(GtkToggleButton *btn, gpointer data)
{
    Battery *b = data;
    gboolean active = gtk_toggle_button_get_active(btn);
    gtk_widget_queue_draw(b->glyph);     /* glifo nero/bianco secondo lo stato */

    if (active) {
        battery_refresh_popover(b);
        gtk_popover_popup(GTK_POPOVER(b->popover));
    } else {
        gtk_popover_popdown(GTK_POPOVER(b->popover));
    }
}

/* ---- Ciclo di aggiornamento --------------------------------------------- */

static void battery_update(Battery *b)
{
    if (b_capacity < 0) {
        gtk_widget_set_visible(b->button, FALSE);
        return;
    }
    gtk_widget_set_visible(b->button, TRUE);

    char txt[8];
    g_snprintf(txt, sizeof txt, "%d%%", b_capacity);
    gtk_label_set_text(GTK_LABEL(b->label), txt);
    gtk_widget_queue_draw(b->glyph);

    /* Se il popover e' aperto, tieni aggiornati anche i suoi valori. */
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b->button)))
        battery_refresh_popover(b);
}

static gboolean battery_tick(gpointer data)
{
    battery_read();
    battery_update((Battery *) data);
    return G_SOURCE_CONTINUE;
}

GtkWidget *battery_new(void)
{
    b_dir = battery_find_dir();
    battery_read();

    Battery *b = g_new0(Battery, 1);

    /* Toggle button = pill: hover -> bolla semitrasparente, checked -> bianca. */
    b->button = gtk_toggle_button_new();
    gtk_widget_add_css_class(b->button, "battery");
    gtk_widget_set_valign(b->button, GTK_ALIGN_CENTER);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);

    b->label = gtk_label_new(NULL);
    gtk_widget_add_css_class(b->label, "battery-pct");
    gtk_box_append(GTK_BOX(content), b->label);

    b->glyph = gtk_drawing_area_new();
    gtk_widget_add_css_class(b->glyph, "battery-glyph");
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(b->glyph), 24);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(b->glyph), 15);
    gtk_widget_set_valign(b->glyph, GTK_ALIGN_CENTER);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(b->glyph),
                                   glyph_draw, NULL, NULL);
    gtk_box_append(GTK_BOX(content), b->glyph);

    gtk_button_set_child(GTK_BUTTON(b->button), content);

    battery_build_popover(b);
    g_signal_connect(b->button,  "toggled", G_CALLBACK(on_toggled),    b);
    g_signal_connect(b->popover, "closed",  G_CALLBACK(on_pop_closed), b);

    battery_update(b);   /* stato iniziale + visibilita' */

    guint id = g_timeout_add_seconds(BAT_POLL_SEC, battery_tick, b);
    g_signal_connect_swapped(b->button, "destroy",
                             G_CALLBACK(g_source_remove),
                             GUINT_TO_POINTER(id));
    /* Il popover e' parentato al button via set_parent: va sparentato a mano
     * quando il button viene distrutto. */
    g_signal_connect_swapped(b->button, "destroy",
                             G_CALLBACK(gtk_widget_unparent), b->popover);
    g_object_set_data_full(G_OBJECT(b->button), "battery", b, g_free);

    return b->button;
}
