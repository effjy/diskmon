// diskmon — real-time disk I/O monitor (GTK4 / C++)
//
// Reads /proc/diskstats to compute per-device read/write throughput, draws a
// live Cairo graph in a Tokyo Night ("Aurora") theme, and reports session
// peak / average stats. Inspired by the layout of the "usage" network monitor.

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define DISKMON_VERSION "1.0.2"

static const int   HISTORY_MAX  = 60;   // points kept in the graph
static const long  SECTOR_BYTES = 512;  // /proc/diskstats counts 512B sectors

// ---------------------------------------------------------------------------
// Per-device raw counters from /proc/diskstats
// ---------------------------------------------------------------------------
struct DiskCounters {
    uint64_t read_bytes  = 0;
    uint64_t write_bytes = 0;
    bool     valid       = false;
};

// Read counters for one device name (e.g. "nvme0n1"). Returns valid=false if
// the device is not currently present in /proc/diskstats.
static DiskCounters read_counters(const std::string &dev) {
    DiskCounters c;
    std::ifstream f("/proc/diskstats");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        unsigned major, minor;
        std::string name;
        uint64_t rd_ios, rd_merges, rd_sectors, rd_ticks;
        uint64_t wr_ios, wr_merges, wr_sectors;
        if (!(ss >> major >> minor >> name
                 >> rd_ios >> rd_merges >> rd_sectors >> rd_ticks
                 >> wr_ios >> wr_merges >> wr_sectors))
            continue;
        if (name == dev) {
            c.read_bytes  = rd_sectors * SECTOR_BYTES;
            c.write_bytes = wr_sectors * SECTOR_BYTES;
            c.valid       = true;
            break;
        }
    }
    return c;
}

// Enumerate whole block devices worth monitoring (skip loop/ram/zram).
static std::vector<std::string> list_devices() {
    std::vector<std::string> out;
    GError *err = nullptr;
    GDir *d = g_dir_open("/sys/block", 0, &err);
    if (!d) {
        if (err) g_error_free(err);
        return out;
    }
    const char *name;
    while ((name = g_dir_read_name(d))) {
        std::string n(name);
        if (n.rfind("loop", 0) == 0) continue;
        if (n.rfind("ram", 0) == 0)  continue;
        if (n.rfind("zram", 0) == 0) continue;
        out.push_back(n);
    }
    g_dir_close(d);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
struct AppState {
    GtkApplication *app = nullptr;
    GtkWidget *window        = nullptr;
    GtkWidget *dropdown      = nullptr;
    GtkWidget *drawing_area  = nullptr;

    GtkWidget *lbl_read_speed  = nullptr;
    GtkWidget *lbl_write_speed = nullptr;
    GtkWidget *lbl_read_total  = nullptr;
    GtkWidget *lbl_write_total = nullptr;
    GtkWidget *lbl_duration    = nullptr;
    GtkWidget *lbl_peak_read   = nullptr;
    GtkWidget *lbl_peak_write  = nullptr;
    GtkWidget *lbl_avg_read    = nullptr;
    GtkWidget *lbl_avg_write   = nullptr;
    GtkWidget *lbl_dev_val     = nullptr;

    std::vector<std::string> devices;
    std::string current_device;

    // Throughput bookkeeping
    DiskCounters last;
    gint64       last_time_us = 0;     // monotonic microseconds

    DiskCounters initial;              // counters when device was selected
    gint64       selection_time_us = 0;

    double peak_read  = 0.0;
    double peak_write = 0.0;
    double sum_read   = 0.0;
    double sum_write  = 0.0;
    uint64_t ticks    = 0;

    double read_hist[HISTORY_MAX]  = {0};
    double write_hist[HISTORY_MAX] = {0};
    int    hist_len = 0;
};

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
static std::string fmt_speed(double bps) {
    const char *u[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
    int i = 0;
    while (bps >= 1024.0 && i < 4) { bps /= 1024.0; i++; }
    char buf[48];
    snprintf(buf, sizeof(buf), "%.1f %s", bps, u[i]);
    return buf;
}

static std::string fmt_bytes(double b) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; i++; }
    char buf[48];
    snprintf(buf, sizeof(buf), "%.2f %s", b, u[i]);
    return buf;
}

static std::string fmt_duration(double s) {
    int total = (int)s;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int sec = total % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, sec);
    return buf;
}

// ---------------------------------------------------------------------------
// Cairo graph (Tokyo Night)
// ---------------------------------------------------------------------------
static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,   G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,       3 * G_PI / 2);
    cairo_close_path(cr);
}

static void build_smooth_area(cairo_t *cr, const double *series, int len,
                              double dx, double max_speed, int height) {
    if (len < 2) return;
    double px[HISTORY_MAX], py[HISTORY_MAX];
    for (int i = 0; i < len; i++) {
        px[i] = i * dx;
        double y = height - (series[i] / max_speed) * height;
        if (y < 1.5) y = 1.5;
        if (y > height) y = height;
        py[i] = y;
    }
    cairo_move_to(cr, px[0], (double)height);
    cairo_line_to(cr, px[0], py[0]);
    for (int i = 0; i < len - 1; i++) {
        double x0 = px[i > 0 ? i - 1 : i],            y0 = py[i > 0 ? i - 1 : i];
        double x1 = px[i],                            y1 = py[i];
        double x2 = px[i + 1],                        y2 = py[i + 1];
        double x3 = px[i + 2 < len ? i + 2 : i + 1],  y3 = py[i + 2 < len ? i + 2 : i + 1];
        double c1x = x1 + (x2 - x0) / 6.0, c1y = y1 + (y2 - y0) / 6.0;
        double c2x = x2 - (x3 - x1) / 6.0, c2y = y2 - (y3 - y1) / 6.0;
        cairo_curve_to(cr, c1x, c1y, c2x, c2y, x2, y2);
    }
    cairo_line_to(cr, px[len - 1], (double)height);
    cairo_close_path(cr);
}

static void draw_series_line(cairo_t *cr, const double *series, int len,
                             double dx, double max_speed, int height) {
    if (len < 2) return;
    double px[HISTORY_MAX], py[HISTORY_MAX];
    for (int i = 0; i < len; i++) {
        px[i] = i * dx;
        double y = height - (series[i] / max_speed) * height;
        if (y < 1.5) y = 1.5;
        if (y > height) y = height;
        py[i] = y;
    }
    cairo_move_to(cr, px[0], py[0]);
    for (int i = 0; i < len - 1; i++) {
        double x0 = px[i > 0 ? i - 1 : i],            y0 = py[i > 0 ? i - 1 : i];
        double x1 = px[i],                            y1 = py[i];
        double x2 = px[i + 1],                        y2 = py[i + 1];
        double x3 = px[i + 2 < len ? i + 2 : i + 1],  y3 = py[i + 2 < len ? i + 2 : i + 1];
        double c1x = x1 + (x2 - x0) / 6.0, c1y = y1 + (y2 - y0) / 6.0;
        double c2x = x2 - (x3 - x1) / 6.0, c2y = y2 - (y3 - y1) / 6.0;
        cairo_curve_to(cr, c1x, c1y, c2x, c2y, x2, y2);
    }
}

static void on_draw(GtkDrawingArea *, cairo_t *cr, int width, int height,
                    gpointer data) {
    AppState *s = static_cast<AppState *>(data);

    // Clip to a rounded panel
    rounded_rect(cr, 0, 0, width, height, 12);
    cairo_clip_preserve(cr);

    // Background gradient
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, height);
    cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.066, 0.072, 0.105);
    cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.043, 0.047, 0.078);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    // Grid
    cairo_set_source_rgba(cr, 0.32, 0.36, 0.52, 0.22);
    cairo_set_line_width(cr, 1.0);
    for (int i = 1; i < 4; i++) {
        double y = height * i / 4.0;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, width, y);
        cairo_stroke(cr);
    }
    for (int i = 1; i < 6; i++) {
        double x = width * i / 6.0;
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x, height);
        cairo_stroke(cr);
    }

    // Determine scale
    double max_speed = 1024.0;  // at least 1 KB/s
    for (int i = 0; i < s->hist_len; i++) {
        max_speed = std::max(max_speed, s->read_hist[i]);
        max_speed = std::max(max_speed, s->write_hist[i]);
    }
    max_speed *= 1.15;

    // Scale labels
    cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.0);
    cairo_set_source_rgba(cr, 0.55, 0.58, 0.72, 0.9);
    std::string top  = fmt_speed(max_speed);
    std::string half = fmt_speed(max_speed / 2.0);
    cairo_move_to(cr, 10, 16);                 cairo_show_text(cr, top.c_str());
    cairo_move_to(cr, 10, height / 2.0 + 4);   cairo_show_text(cr, half.c_str());
    cairo_move_to(cr, 10, height - 9);         cairo_show_text(cr, "0.0 B/s");

    if (s->hist_len >= 2) {
        double dx = (double)width / (HISTORY_MAX - 1);

        struct Series { const double *data; double r, g, b; } series[2] = {
            { s->read_hist,  0.619, 0.807, 0.415 },  // #9ece6a green
            { s->write_hist, 0.968, 0.462, 0.556 },  // #f7768e red
        };

        for (auto &se : series) {
            // Filled area
            build_smooth_area(cr, se.data, s->hist_len, dx, max_speed, height);
            cairo_pattern_t *fill = cairo_pattern_create_linear(0, 0, 0, height);
            cairo_pattern_add_color_stop_rgba(fill, 0.0, se.r, se.g, se.b, 0.30);
            cairo_pattern_add_color_stop_rgba(fill, 1.0, se.r, se.g, se.b, 0.0);
            cairo_set_source(cr, fill);
            cairo_fill(cr);
            cairo_pattern_destroy(fill);

            // Glow + crisp line
            draw_series_line(cr, se.data, s->hist_len, dx, max_speed, height);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_set_source_rgba(cr, se.r, se.g, se.b, 0.30);
            cairo_set_line_width(cr, 6.0);
            cairo_stroke_preserve(cr);
            cairo_set_source_rgb(cr, se.r, se.g, se.b);
            cairo_set_line_width(cr, 2.0);
            cairo_stroke(cr);

            // Leading dot
            double lx = (s->hist_len - 1) * dx;
            double ly = height - (se.data[s->hist_len - 1] / max_speed) * height;
            if (ly < 1.5) ly = 1.5;
            if (ly > height) ly = height;
            cairo_set_source_rgba(cr, se.r, se.g, se.b, 0.25);
            cairo_arc(cr, lx, ly, 5.0, 0, 2 * G_PI); cairo_fill(cr);
            cairo_set_source_rgb(cr, se.r, se.g, se.b);
            cairo_arc(cr, lx, ly, 2.5, 0, 2 * G_PI); cairo_fill(cr);
        }
    }

    // Border
    cairo_reset_clip(cr);
    rounded_rect(cr, 0.5, 0.5, width - 1, height - 1, 12);
    cairo_set_source_rgba(cr, 0.48, 0.55, 0.86, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
}

// ---------------------------------------------------------------------------
// Reset session aggregates (on device switch / startup)
// ---------------------------------------------------------------------------
static void reset_session(AppState *s) {
    s->initial = read_counters(s->current_device);
    s->last    = s->initial;
    s->last_time_us      = g_get_monotonic_time();
    s->selection_time_us = s->last_time_us;
    s->peak_read = s->peak_write = 0.0;
    s->sum_read  = s->sum_write  = 0.0;
    s->ticks = 0;
    s->hist_len = 0;
    for (int i = 0; i < HISTORY_MAX; i++) {
        s->read_hist[i] = s->write_hist[i] = 0.0;
    }
    if (s->lbl_dev_val)
        gtk_label_set_text(GTK_LABEL(s->lbl_dev_val), s->current_device.c_str());
}

static void push_history(double *hist, int *len, double v) {
    if (*len < HISTORY_MAX) {
        hist[*len] = v;
        (*len)++;
    } else {
        memmove(hist, hist + 1, (HISTORY_MAX - 1) * sizeof(double));
        hist[HISTORY_MAX - 1] = v;
    }
}

// ---------------------------------------------------------------------------
// Periodic tick
// ---------------------------------------------------------------------------
static gboolean on_tick(gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    if (s->current_device.empty()) return G_SOURCE_CONTINUE;

    DiskCounters now = read_counters(s->current_device);
    gint64 now_us = g_get_monotonic_time();
    if (!now.valid) return G_SOURCE_CONTINUE;

    double dt = (now_us - s->last_time_us) / 1e6;
    if (dt <= 0.0) dt = 1.0;

    double rspeed = (now.read_bytes  - s->last.read_bytes)  / dt;
    double wspeed = (now.write_bytes - s->last.write_bytes) / dt;
    if (rspeed < 0) rspeed = 0;
    if (wspeed < 0) wspeed = 0;

    s->last = now;
    s->last_time_us = now_us;

    // Aggregates
    s->peak_read  = std::max(s->peak_read,  rspeed);
    s->peak_write = std::max(s->peak_write, wspeed);
    s->sum_read  += rspeed;
    s->sum_write += wspeed;
    s->ticks++;

    // Push read first (advances hist_len), then mirror write into the same
    // logical slot so both series stay index-aligned.
    bool full = (s->hist_len == HISTORY_MAX);
    push_history(s->read_hist, &s->hist_len, rspeed);
    if (full) {
        memmove(s->write_hist, s->write_hist + 1, (HISTORY_MAX - 1) * sizeof(double));
        s->write_hist[HISTORY_MAX - 1] = wspeed;
    } else {
        s->write_hist[s->hist_len - 1] = wspeed;
    }

    // Totals since selection
    double read_total  = (double)(now.read_bytes  - s->initial.read_bytes);
    double write_total = (double)(now.write_bytes - s->initial.write_bytes);
    double duration    = (now_us - s->selection_time_us) / 1e6;
    double avg_read    = s->ticks ? s->sum_read  / s->ticks : 0.0;
    double avg_write   = s->ticks ? s->sum_write / s->ticks : 0.0;

    gtk_label_set_text(GTK_LABEL(s->lbl_read_speed),  fmt_speed(rspeed).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_write_speed), fmt_speed(wspeed).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_read_total),  fmt_bytes(read_total).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_write_total), fmt_bytes(write_total).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_duration),    fmt_duration(duration).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_peak_read),   fmt_speed(s->peak_read).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_peak_write),  fmt_speed(s->peak_write).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_avg_read),    fmt_speed(avg_read).c_str());
    gtk_label_set_text(GTK_LABEL(s->lbl_avg_write),   fmt_speed(avg_write).c_str());

    gtk_widget_queue_draw(s->drawing_area);
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Device selection (GtkDropDown)
// ---------------------------------------------------------------------------
static void on_device_changed(GtkDropDown *dd, GParamSpec *, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    guint idx = gtk_drop_down_get_selected(dd);
    if (idx == GTK_INVALID_LIST_POSITION || idx >= s->devices.size()) return;
    s->current_device = s->devices[idx];
    reset_session(s);
}

// ---------------------------------------------------------------------------
// About dialog — with the GTK4 "everything is selected" workaround
// ---------------------------------------------------------------------------
// GtkAboutDialog in GTK4 maps with its name/copyright labels showing a stray
// text selection. Walk the widget tree and clear any GtkLabel selection once
// it has been realized.
static void clear_label_selection(GtkWidget *w) {
    if (GTK_IS_LABEL(w)) {
        gtk_label_select_region(GTK_LABEL(w), 0, 0);
    }
    for (GtkWidget *c = gtk_widget_get_first_child(w); c;
         c = gtk_widget_get_next_sibling(c)) {
        clear_label_selection(c);
    }
}

static gboolean about_unselect_idle(gpointer data) {
    GtkWidget *dialog = static_cast<GtkWidget *>(data);
    if (GTK_IS_WIDGET(dialog)) {
        clear_label_selection(dialog);
        // Make sure no label keeps the keyboard focus / selection anchor.
        gtk_window_set_focus(GTK_WINDOW(dialog), nullptr);
    }
    return G_SOURCE_REMOVE;
}

static void show_about(GtkWidget *, gpointer data) {
    AppState *s = static_cast<AppState *>(data);

    GtkWidget *dialog = gtk_about_dialog_new();
    GtkAboutDialog *ad = GTK_ABOUT_DIALOG(dialog);
    gtk_about_dialog_set_program_name(ad, "Disk Monitor");
    gtk_about_dialog_set_version(ad, DISKMON_VERSION);
    gtk_about_dialog_set_comments(ad,
        "Real-time disk I/O monitor for Linux.\n"
        "Live read/write throughput, peaks and session averages.");
    gtk_about_dialog_set_copyright(ad, "© 2026 Jean-Francois Lachance-Caumartin");
    gtk_about_dialog_set_license_type(ad, GTK_LICENSE_MIT_X11);
    gtk_about_dialog_set_website(ad, "https://github.com/effjy/diskmon");
    gtk_about_dialog_set_website_label(ad, "Project home");
    // Uses the globally-installed icon (see Makefile install target).
    gtk_about_dialog_set_logo_icon_name(ad, "diskmon");

    const char *authors[] = { "Jean-Francois Lachance-Caumartin", nullptr };
    gtk_about_dialog_set_authors(ad, authors);

    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(s->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    // Clear the spurious selection after the dialog is laid out.
    g_signal_connect(dialog, "map", G_CALLBACK(+[](GtkWidget *w, gpointer) {
        g_idle_add(about_unselect_idle, w);
    }), nullptr);

    gtk_window_present(GTK_WINDOW(dialog));
}

// ---------------------------------------------------------------------------
// UI construction helpers
// ---------------------------------------------------------------------------
static GtkWidget *make_stat_card(const char *title, const char *accent_class,
                                 const char *val_class, GtkWidget **out_val,
                                 const char *sub_text, GtkWidget **out_sub) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_add_css_class(card, accent_class);
    gtk_widget_set_hexpand(card, TRUE);

    GtkWidget *lt = gtk_label_new(title);
    gtk_widget_add_css_class(lt, "stat-title");
    gtk_widget_set_halign(lt, GTK_ALIGN_START);

    GtkWidget *lv = gtk_label_new("0.0 B/s");
    gtk_widget_add_css_class(lv, "stat-val");
    gtk_widget_add_css_class(lv, val_class);
    gtk_widget_set_halign(lv, GTK_ALIGN_START);
    *out_val = lv;

    gtk_box_append(GTK_BOX(card), lt);
    gtk_box_append(GTK_BOX(card), lv);

    if (sub_text) {
        GtkWidget *ls = gtk_label_new(sub_text);
        gtk_widget_add_css_class(ls, "stat-sub");
        gtk_widget_set_halign(ls, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(card), ls);
        if (out_sub) *out_sub = ls;
    }
    return card;
}

static GtkWidget *make_mini_stat(const char *title, GtkWidget **out_val) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(box, TRUE);
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "grid-label-title");
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    GtkWidget *v = gtk_label_new("—");
    gtk_widget_add_css_class(v, "grid-label-val");
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    *out_val = v;
    gtk_box_append(GTK_BOX(box), t);
    gtk_box_append(GTK_BOX(box), v);
    return box;
}

// ---------------------------------------------------------------------------
// Application activate
// ---------------------------------------------------------------------------
static void activate(GtkApplication *app, gpointer data) {
    AppState *s = static_cast<AppState *>(data);
    s->app = app;

    // ---- CSS (Aurora · Tokyo Night Storm) ----
    GtkCssProvider *css = gtk_css_provider_new();
    const char *style =
        "window {"
        "  background-image: radial-gradient(circle at 12% -8%, #2a2f45 0%, #1a1b26 42%),"
        "                    radial-gradient(circle at 110% 120%, #232a47 0%, rgba(26,27,38,0) 55%);"
        "  background-color: #16161e;"
        "  color: #a9b1d6;"
        "  font-family: 'Inter', 'Cantarell', sans-serif;"
        "}\n"
        ".main-container { padding: 18px; }\n"
        ".card {"
        "  background-image: linear-gradient(145deg, rgba(41,46,66,0.96), rgba(31,35,53,0.96));"
        "  border: 1px solid rgba(122,162,247,0.16);"
        "  border-radius: 14px; padding: 18px;"
        "}\n"
        ".card-read  { border-left: 3px solid #9ece6a; }\n"
        ".card-write { border-left: 3px solid #f7768e; }\n"
        ".stat-title { font-size: 11px; font-weight: 800; color: #6b74a0; letter-spacing: 1.4px; }\n"
        ".stat-val {"
        "  font-size: 30px; font-weight: 800; margin-top: 4px; margin-bottom: 4px;"
        "  text-shadow: 0 0 18px rgba(122,162,247,0.28);"
        "}\n"
        ".stat-val-read  { color: #9ece6a; text-shadow: 0 0 20px rgba(158,206,106,0.45); }\n"
        ".stat-val-write { color: #f7768e; text-shadow: 0 0 20px rgba(247,118,142,0.45); }\n"
        ".stat-sub { font-size: 12px; color: #7e85ad; font-weight: 600; }\n"
        ".graph-title { font-size: 12px; font-weight: 800; color: #7aa2f7; letter-spacing: 1.6px; margin-bottom: 8px; }\n"
        ".grid-label-title { font-size: 11px; font-weight: 800; color: #6b74a0; letter-spacing: 1px; }\n"
        ".grid-label-val   { font-size: 15px; font-weight: 700; color: #c0caf5; }\n"
        ".panel {"
        "  background-image: linear-gradient(145deg, rgba(41,46,66,0.96), rgba(31,35,53,0.96));"
        "  border: 1px solid rgba(122,162,247,0.16); border-radius: 14px; padding: 16px;"
        "}\n"
        "headerbar {"
        "  background-image: linear-gradient(to bottom, #2a2f45, #1d2030);"
        "  border-bottom: 1px solid rgba(122,162,247,0.25); padding: 8px 10px;"
        "}\n"
        "headerbar .title { font-weight: 800; color: #c0caf5; letter-spacing: 0.4px; }\n"
        "dropdown {"
        "  background-image: linear-gradient(145deg, #2a2f45, #24283b);"
        "  border: 1px solid rgba(122,162,247,0.28); color: #c0caf5;"
        "  border-radius: 9px; padding: 2px 6px;"
        "}\n"
        "dropdown:hover { border-color: #7aa2f7; }\n"
        "dropdown button { background: none; border: none; box-shadow: none; color: #c0caf5; }\n"
        "button {"
        "  background-image: linear-gradient(145deg, #2a2f45, #24283b);"
        "  border: 1px solid rgba(122,162,247,0.28); color: #c0caf5;"
        "  border-radius: 9px; padding: 4px 10px; transition: all 160ms ease;"
        "}\n"
        "button:hover {"
        "  background-image: linear-gradient(145deg, #7aa2f7, #5a7fe0);"
        "  border-color: #7aa2f7; color: #16161e;"
        "}\n"
        "tooltip { background-color: #1f2335; color: #c0caf5; border: 1px solid #414868; }\n";
    gtk_css_provider_load_from_string(css, style);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    // ---- Window ----
    s->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(s->window), "Disk Monitor");
    gtk_window_set_default_size(GTK_WINDOW(s->window), 700, 520);
    gtk_window_set_icon_name(GTK_WINDOW(s->window), "diskmon");

    // ---- Header bar ----
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(s->window), header);

    // Device dropdown
    s->devices = list_devices();
    std::vector<const char *> dev_cstr;
    for (auto &d : s->devices) dev_cstr.push_back(d.c_str());
    dev_cstr.push_back(nullptr);
    s->dropdown = gtk_drop_down_new_from_strings(dev_cstr.data());
    gtk_widget_set_tooltip_text(s->dropdown, "Block device to monitor");
    g_signal_connect(s->dropdown, "notify::selected",
                     G_CALLBACK(on_device_changed), s);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), s->dropdown);

    // About button
    GtkWidget *about_btn = gtk_button_new_from_icon_name("help-about-symbolic");
    gtk_widget_set_tooltip_text(about_btn, "About Disk Monitor");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(show_about), s);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about_btn);

    // ---- Root container ----
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(root, "main-container");
    gtk_window_set_child(GTK_WINDOW(s->window), root);

    // Stat cards row
    GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *read_card = make_stat_card("READ", "card-read", "stat-val-read",
                                          &s->lbl_read_speed, "Total: 0 B",
                                          &s->lbl_read_total);
    GtkWidget *write_card = make_stat_card("WRITE", "card-write", "stat-val-write",
                                           &s->lbl_write_speed, "Total: 0 B",
                                           &s->lbl_write_total);
    gtk_box_append(GTK_BOX(cards), read_card);
    gtk_box_append(GTK_BOX(cards), write_card);
    gtk_box_append(GTK_BOX(root), cards);

    // Graph panel
    GtkWidget *graph_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(graph_panel, "panel");
    gtk_widget_set_vexpand(graph_panel, TRUE);
    GtkWidget *gtitle = gtk_label_new("THROUGHPUT  ·  read / write");
    gtk_widget_add_css_class(gtitle, "graph-title");
    gtk_widget_set_halign(gtitle, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(graph_panel), gtitle);

    s->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_vexpand(s->drawing_area, TRUE);
    gtk_widget_set_hexpand(s->drawing_area, TRUE);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(s->drawing_area), 180);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(s->drawing_area),
                                   on_draw, s, nullptr);
    gtk_box_append(GTK_BOX(graph_panel), s->drawing_area);
    gtk_box_append(GTK_BOX(root), graph_panel);

    // Mini stats grid
    GtkWidget *grid_panel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(grid_panel, "panel");
    gtk_box_append(GTK_BOX(grid_panel), make_mini_stat("DEVICE",     &s->lbl_dev_val));
    gtk_box_append(GTK_BOX(grid_panel), make_mini_stat("DURATION",   &s->lbl_duration));
    gtk_box_append(GTK_BOX(grid_panel), make_mini_stat("PEAK READ",  &s->lbl_peak_read));
    gtk_box_append(GTK_BOX(grid_panel), make_mini_stat("PEAK WRITE", &s->lbl_peak_write));
    gtk_box_append(GTK_BOX(grid_panel), make_mini_stat("AVG READ",   &s->lbl_avg_read));
    gtk_box_append(GTK_BOX(grid_panel), make_mini_stat("AVG WRITE",  &s->lbl_avg_write));
    gtk_box_append(GTK_BOX(root), grid_panel);

    // Initial device
    if (!s->devices.empty()) {
        s->current_device = s->devices[0];
        reset_session(s);
    }

    g_timeout_add(1000, on_tick, s);
    gtk_window_present(GTK_WINDOW(s->window));
}

int main(int argc, char **argv) {
    AppState state;
    GtkApplication *app = gtk_application_new("org.effjy.diskmon",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
