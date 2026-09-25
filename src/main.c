#define _POSIX_C_SOURCE 200809L
#include "scan.h"
#include "report.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SIDE = 340, TOP = 64, BOTTOM = 34, ROW = 23, PAD = 12 };

typedef struct { int x, y, w, h; } Rect;
typedef struct {
    Display *display;
    Window window;
    GC gc;
    XFontStruct *font;
    Atom close_atom;
    unsigned long colors[16];
    int width, height, scroll, scanning, quit;
    ScanOptions scan_options;
    Node *root, *focus, *selected;
    ScanStats stats;
    char *path;
    char progress_path[240];
    char notice[512];
    Time last_click;
    Node *last_hit;
    unsigned layout_generation;
    int64_t last_progress_draw_ms;
} App;

extern char **environ;

static void format_size(uint64_t size, char *out, size_t capacity) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double n = (double)size;
    int unit = 0;
    while (n >= 1024.0 && unit < 6) { n /= 1024.0; unit++; }
    if (unit == 0) snprintf(out, capacity, "%" PRIu64 " B", size);
    else snprintf(out, capacity, "%.1f %s", n, units[unit]);
}

static void color(App *app, int slot) {
    XSetForeground(app->display, app->gc, app->colors[slot]);
}

static void fill(App *app, int slot, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    color(app, slot);
    XFillRectangle(app->display, app->window, app->gc, r.x, r.y,
                   (unsigned)r.w, (unsigned)r.h);
}

static void line(App *app, int slot, int x, int y, int w, int h) {
    if (w <= 1 || h <= 1) return;
    color(app, slot);
    XDrawRectangle(app->display, app->window, app->gc, x, y,
                   (unsigned)(w - 1), (unsigned)(h - 1));
}

static void label(App *app, int slot, int x, int y, int max_width, const char *s) {
    if (max_width < 10 || !s) return;
    size_t len = strlen(s);
    if (len > 1024) len = 1024;
    while (len && XTextWidth(app->font, s, (int)len) > max_width) len--;
    color(app, slot);
    XDrawString(app->display, app->window, app->gc, x, y, s, (int)len);
}

static unsigned hash_type(const Node *node) {
    if (node->is_symlink) return 14;
    const char *dot = strrchr(node->name, '.');
    const unsigned char *s = (const unsigned char *)(dot && dot[1] ? dot + 1 : node->name);
    unsigned h = 2166136261u;
    for (int i = 0; s[i] && i < 24; i++) h = (h ^ s[i]) * 16777619u;
    return 6 + h % 8;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void layout_node(App *app, Node *node, Rect r, unsigned depth);

static void layout_group(App *app, Node **items, size_t lo, size_t hi, Rect r,
                         uint64_t total, unsigned depth) {
    if (lo >= hi || r.w < 1 || r.h < 1 || total == 0) return;
    if (hi - lo == 1) { layout_node(app, items[lo], r, depth); return; }
    /* Tiny groups cannot show useful detail and can contain thousands of nodes. */
    if ((int64_t)r.w * r.h <= 16) { fill(app, hash_type(items[lo]), r); return; }
    uint64_t half = total / 2, left = 0;
    size_t mid = lo;
    while (mid + 1 < hi && (left < half || mid == lo)) {
        left += items[mid]->size;
        mid++;
    }
    if (mid == lo) mid = lo + 1;
    if (left == 0 || left >= total) {
        left = 0;
        for (size_t i = lo; i < mid; i++) left += items[i]->size;
    }
    int wide = r.w >= r.h;
    int extent = wide ? r.w : r.h;
    int split = (int)((long double)extent * left / total);
    if (split < 1) split = 1;
    if (split >= extent) split = extent - 1;
    if (split <= 0) { layout_node(app, items[lo], r, depth); return; }
    Rect a = r, b = r;
    if (wide) { a.w = split; b.x += split; b.w -= split; }
    else { a.h = split; b.y += split; b.h -= split; }
    layout_group(app, items, lo, mid, a, left, depth);
    layout_group(app, items, mid, hi, b, total - left, depth);
}

static void layout_node(App *app, Node *node, Rect r, unsigned depth) {
    node->x = r.x; node->y = r.y; node->w = r.w; node->h = r.h;
    node->layout_generation = app->layout_generation;
    if (r.w < 1 || r.h < 1) return;
    if (!node->is_dir || node->child_count == 0 || depth > 1000 ||
        (int64_t)r.w * r.h <= 16) {
        fill(app, node->is_dir ? 3 : hash_type(node), r);
        if (r.w > 4 && r.h > 4) line(app, 2, r.x, r.y, r.w, r.h);
        return;
    }
    fill(app, 3, r);
    size_t nonzero = 0;
    while (nonzero < node->child_count && node->children[nonzero]->size) nonzero++;
    uint64_t children_size = node->size - node->self_size;
    Rect children = r;
    if (node->self_size && children_size) {
        if (r.w >= r.h) {
            children.w = (int)((long double)r.w * children_size / node->size);
            if (children.w < 1) children.w = 1;
        } else {
            children.h = (int)((long double)r.h * children_size / node->size);
            if (children.h < 1) children.h = 1;
        }
    }
    layout_group(app, node->children, 0, nonzero, children, children_size, depth + 1);
    if (r.w > 5 && r.h > 5) line(app, 4, r.x, r.y, r.w, r.h);
}

static Node *hit(Node *node, int x, int y, unsigned generation) {
    if (!node || node->layout_generation != generation ||
        x < node->x || y < node->y || x >= node->x + node->w ||
        y >= node->y + node->h || node->w <= 0 || node->h <= 0) return NULL;
    for (size_t i = 0; i < node->child_count; i++) {
        Node *found = hit(node->children[i], x, y, generation);
        if (found) return found;
    }
    return node;
}

static int visible_rows(const App *app) {
    int rows = (app->height - TOP - BOTTOM - 82) / ROW;
    return rows > 0 ? rows : 0;
}

static void draw(App *app) {
    Rect full = {0, 0, app->width, app->height};
    fill(app, 0, full);
    fill(app, 1, (Rect){0, 0, app->width, TOP});
    label(app, 5, PAD, 25, app->width - 2 * PAD,
          "DebBarStat  |  A size mode  |  E export  |  Ctrl+click folder");
    if (app->scanning) {
        char info[260];
        snprintf(info, sizeof(info), "Scanning  %" PRIu64 " files  /  %" PRIu64 " directories",
                 app->stats.files, app->stats.directories);
        label(app, 4, PAD, 48, app->width - 2 * PAD, info);
        label(app, 4, PAD, TOP + 32, app->width - 2 * PAD, app->progress_path);
        XFlush(app->display);
        return;
    }
    if (!app->focus) { XFlush(app->display); return; }
    char *focus_path = node_path(app->focus);
    label(app, 4, PAD, 48, app->width - 2 * PAD, focus_path);
    free(focus_path);
    fill(app, 1, (Rect){0, TOP, SIDE, app->height - TOP - BOTTOM});
    char size[64], linebuf[512];
    format_size(app->focus->size, size, sizeof(size));
    snprintf(linebuf, sizeof(linebuf), "%s: %s",
             app->scan_options.apparent_size ? "Apparent" : "Allocated", size);
    label(app, 5, PAD, TOP + 25, SIDE - 2 * PAD, linebuf);
    snprintf(linebuf, sizeof(linebuf), "%" PRIu64 " files / %" PRIu64
             " dirs / %" PRIu64 " linked copies", app->stats.files,
             app->stats.directories, app->stats.hardlink_duplicates);
    label(app, 4, PAD, TOP + 48, SIDE - 2 * PAD, linebuf);
    label(app, 4, PAD, TOP + 71, SIDE - 2 * PAD, "Largest entries");
    int rows = visible_rows(app);
    if (app->scroll < 0) app->scroll = 0;
    if ((size_t)app->scroll > app->focus->child_count) app->scroll = (int)app->focus->child_count;
    for (int j = 0; j < rows && (size_t)(j + app->scroll) < app->focus->child_count; j++) {
        Node *child = app->focus->children[j + app->scroll];
        int y = TOP + 94 + j * ROW;
        if (child == app->selected) fill(app, 3, (Rect){6, y - 15, SIDE - 12, ROW});
        format_size(child->size, size, sizeof(size));
        snprintf(linebuf, sizeof(linebuf), "%s %s", child->is_dir ? ">" :
                 child->is_hardlink_duplicate ? "=" : " ", child->name);
        label(app, child->is_dir ? 5 : 4, PAD, y, SIDE - 110, linebuf);
        label(app, 4, SIDE - 99, y, 92, size);
    }
    Rect map = {SIDE + 6, TOP + 6, app->width - SIDE - 12,
                app->height - TOP - BOTTOM - 12};
    fill(app, 3, map);
    app->layout_generation++;
    if (app->layout_generation == 0) app->layout_generation++;
    if (app->focus->size && map.w > 0 && map.h > 0) layout_node(app, app->focus, map, 0);
    else label(app, 4, map.x + 12, map.y + 24, map.w - 24, "No nonempty files");
    if (app->selected && app->selected->layout_generation == app->layout_generation &&
        app->selected->w > 3 && app->selected->h > 3)
        line(app, 5, app->selected->x, app->selected->y, app->selected->w, app->selected->h);
    fill(app, 1, (Rect){0, app->height - BOTTOM, app->width, BOTTOM});
    if (app->notice[0]) {
        label(app, 5, PAD, app->height - 12, app->width - 2 * PAD, app->notice);
    } else {
        const Node *detail = app->selected ? app->selected : app->focus;
        char *detail_path = node_path(detail);
        format_size(detail->size, size, sizeof(size));
        snprintf(linebuf, sizeof(linebuf), "%s%s  |  %s", size,
                 detail->is_hardlink_duplicate ? " (hard link counted elsewhere)" : "",
                 detail_path ? detail_path : "");
        label(app, 4, PAD, app->height - 12, app->width - 2 * PAD, linebuf);
        free(detail_path);
    }
    XFlush(app->display);
}

static void progress(ScanStats *stats, const char *path, void *context) {
    App *app = context;
    app->stats = *stats;
    snprintf(app->progress_path, sizeof(app->progress_path), "%s", path);
    while (XPending(app->display)) {
        XEvent event;
        XNextEvent(app->display, &event);
        if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == app->close_atom)
            app->quit = 1;
        if (event.type == DestroyNotify) app->quit = 1;
        if (event.type == KeyPress) {
            KeySym key = XLookupKeysym(&event.xkey, 0);
            if (key == XK_q || key == XK_Escape) app->quit = 1;
        }
        if (event.type == ConfigureNotify) {
            app->width = event.xconfigure.width;
            app->height = event.xconfigure.height;
        }
    }
    if (app->quit) stats->cancelled = 1;
    int64_t now = monotonic_ms();
    if (now - app->last_progress_draw_ms >= 120) {
        draw(app);
        app->last_progress_draw_ms = now;
    }
}

static int rescan(App *app) {
    Node *previous_focus = app->focus;
    Node *previous_selection = app->selected;
    ScanStats previous_stats = app->stats;
    int previous_scroll = app->scroll;
    app->scanning = 1;
    app->selected = app->focus = NULL;
    app->scroll = 0;
    app->last_hit = NULL;
    snprintf(app->progress_path, sizeof(app->progress_path), "%s", app->path);
    draw(app);
    app->last_progress_draw_ms = monotonic_ms();
    ScanStats stats;
    Node *next = scan_tree(app->path, app->scan_options, &stats, progress, app);
    app->stats = stats;
    app->scanning = 0;
    if (!next) {
        app->focus = previous_focus;
        app->selected = previous_selection;
        app->stats = previous_stats;
        app->scroll = previous_scroll;
        return -1;
    }
    node_free(app->root);
    app->root = app->focus = next;
    return 0;
}

static int make_window(App *app) {
    app->display = XOpenDisplay(NULL);
    if (!app->display) { fprintf(stderr, "DebBarStat: cannot open X display\n"); return -1; }
    int screen = DefaultScreen(app->display);
    app->width = 1100; app->height = 700;
    app->window = XCreateSimpleWindow(app->display, RootWindow(app->display, screen),
                                      60, 60, app->width, app->height, 0, 0, 0);
    XStoreName(app->display, app->window, "DebBarStat — Disk Visualizer");
    XSelectInput(app->display, app->window, ExposureMask | StructureNotifyMask |
                 KeyPressMask | ButtonPressMask);
    app->close_atom = XInternAtom(app->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app->display, app->window, &app->close_atom, 1);
    app->gc = XCreateGC(app->display, app->window, 0, NULL);
    app->font = XLoadQueryFont(app->display, "fixed");
    if (!app->font) { fprintf(stderr, "DebBarStat: fixed font unavailable\n"); return -1; }
    XSetFont(app->display, app->gc, app->font->fid);
    static const char *hex[] = {
        "#14181e", "#202831", "#10151a", "#304253", "#aab7c5", "#f2f6fa",
        "#4d90a4", "#5eaa81", "#b28a54", "#a76d83", "#647db3", "#a07bb7",
        "#8aa868", "#b46e5d", "#737f89", "#e6c45c"
    };
    Colormap cmap = DefaultColormap(app->display, screen);
    for (int i = 0; i < 16; i++) {
        XColor c;
        if (!XParseColor(app->display, cmap, hex[i], &c) || !XAllocColor(app->display, cmap, &c))
            app->colors[i] = WhitePixel(app->display, screen);
        else app->colors[i] = c.pixel;
    }
    XMapWindow(app->display, app->window);
    return 0;
}

static void open_folder(App *app, const Node *node) {
    const Node *folder = node->is_dir ? node : node->parent;
    char *path = node_path(folder);
    if (!path) {
        snprintf(app->notice, sizeof(app->notice), "Could not build folder path");
        return;
    }
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result == 0) {
        result = posix_spawn_file_actions_addclose(&actions, ConnectionNumber(app->display));
        if (result == 0) {
            char *args[] = {"xdg-open", path, NULL};
            pid_t child;
            result = posix_spawnp(&child, "xdg-open", &actions, NULL, args, environ);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    if (result == 0)
        snprintf(app->notice, sizeof(app->notice), "Opening folder: %s", path);
    else snprintf(app->notice, sizeof(app->notice), "Could not open file manager: %s",
                  strerror(result));
    free(path);
}

static void export_report(App *app) {
    time_t now = time(NULL);
    struct tm when;
    if (now == (time_t)-1 || !localtime_r(&now, &when)) {
        snprintf(app->notice, sizeof(app->notice), "Could not get report timestamp");
        return;
    }
    char stamp[32];
    if (!strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &when)) return;
    for (int suffix = 0; suffix < 1000; suffix++) {
        char path[96];
        snprintf(path, sizeof(path), "debbarstat-%s-%03d.csv", stamp, suffix);
        if (report_export_csv(path, app->root, app->scan_options, &app->stats) == 0) {
            snprintf(app->notice, sizeof(app->notice), "Saved report in current directory: %s",
                     path);
            return;
        }
        if (errno != EEXIST) {
            snprintf(app->notice, sizeof(app->notice), "Report export failed: %s",
                     strerror(errno));
            return;
        }
    }
    snprintf(app->notice, sizeof(app->notice), "Report export failed: too many files");
}

static int handle_key(App *app, XKeyEvent *event) {
    KeySym key = XLookupKeysym(event, 0);
    if (key == XK_q || key == XK_Escape) { app->quit = 1; return 0; }
    if (key == XK_BackSpace || key == XK_Left) {
        int changed = app->focus->parent || app->selected || app->scroll || app->notice[0];
        if (app->focus->parent) app->focus = app->focus->parent;
        app->selected = NULL; app->scroll = 0;
        app->notice[0] = '\0';
        return changed;
    }
    if (key == XK_r || key == XK_R) {
        app->notice[0] = '\0';
        if (rescan(app) != 0) fprintf(stderr, "DebBarStat: rescan failed: %s\n", strerror(errno));
        return 1;
    }
    if (key == XK_a || key == XK_A) {
        app->notice[0] = '\0';
        app->scan_options.apparent_size = !app->scan_options.apparent_size;
        if (rescan(app) != 0) {
            app->scan_options.apparent_size = !app->scan_options.apparent_size;
            fprintf(stderr, "DebBarStat: size-mode scan failed: %s\n", strerror(errno));
        }
        return 1;
    }
    if (key == XK_e || key == XK_E) {
        export_report(app);
        return 1;
    }
    if ((key == XK_Down || key == XK_Up || key == XK_Home || key == XK_End) &&
        app->focus->child_count) {
        Node *previous = app->selected;
        int previous_scroll = app->scroll;
        int had_notice = app->notice[0] != '\0';
        size_t index = app->focus->child_count;
        for (size_t i = 0; i < app->focus->child_count; i++)
            if (app->focus->children[i] == app->selected) { index = i; break; }
        if (key == XK_Home) index = 0;
        else if (key == XK_End) index = app->focus->child_count - 1;
        else if (key == XK_Down) {
            if (index >= app->focus->child_count) index = 0;
            else if (index + 1 < app->focus->child_count) index++;
        } else if (index >= app->focus->child_count) index = 0;
        else if (index > 0) index--;
        app->selected = app->focus->children[index];
        int rows = visible_rows(app);
        if ((int)index < app->scroll) app->scroll = (int)index;
        else if (rows > 0 && (int)index >= app->scroll + rows)
            app->scroll = (int)index - rows + 1;
        app->notice[0] = '\0';
        return previous != app->selected || previous_scroll != app->scroll || had_notice;
    }
    if (key == XK_Return && app->selected && app->selected->is_dir) {
        app->focus = app->selected; app->selected = NULL; app->scroll = 0;
        app->notice[0] = '\0';
        return 1;
    }
    return 0;
}

static int handle_button(App *app, XButtonEvent *event) {
    if (event->button == Button3) {
        int changed = app->focus->parent || app->selected || app->scroll || app->notice[0];
        if (app->focus->parent) app->focus = app->focus->parent;
        app->selected = NULL; app->scroll = 0;
        app->notice[0] = '\0';
        return changed;
    }
    if (event->x < SIDE && event->y >= TOP + 77) {
        if (event->button == Button4 && app->scroll > 0) {
            app->scroll--; app->notice[0] = '\0'; return 1;
        }
        if (event->button == Button5 && (size_t)(app->scroll + 1) < app->focus->child_count) {
            app->scroll++; app->notice[0] = '\0'; return 1;
        }
        if (event->button != Button1) return 0;
        int index = (event->y - (TOP + 79)) / ROW + app->scroll;
        if (index >= 0 && (size_t)index < app->focus->child_count) {
            Node *n = app->focus->children[index];
            if (event->state & ControlMask) { open_folder(app, n); return 1; }
            if (n->is_dir) { app->focus = n; app->selected = NULL; app->scroll = 0; }
            else {
                int changed = app->selected != n || app->notice[0];
                app->selected = n;
                app->notice[0] = '\0';
                return changed;
            }
            app->notice[0] = '\0';
            return 1;
        }
        return 0;
    }
    if (event->button != Button1 || event->x < SIDE || event->y < TOP) return 0;
    Node *n = hit(app->focus, event->x, event->y, app->layout_generation);
    if (!n) return 0;
    if (event->state & ControlMask) { open_folder(app, n); return 1; }
    int changed = app->selected != n || app->notice[0];
    if (n == app->last_hit && event->time - app->last_click < 350) {
        Node *dir = n->is_dir ? n : n->parent;
        if (dir && dir != app->focus) { app->focus = dir; app->scroll = 0; changed = 1; }
        app->selected = NULL;
        changed = 1;
    } else app->selected = n;
    app->last_hit = n;
    app->last_click = event->time;
    app->notice[0] = '\0';
    return changed;
}

static void usage(FILE *out) {
    fputs("Usage: debbarstat [--all-filesystems] [--apparent-size] [--summary | --export CSV] [PATH]\n"
          "  PATH defaults to the current directory. Symlinks are never followed.\n"
          "  By default, mounted filesystems beneath PATH are skipped.\n"
          "  Virtual filesystems are always skipped, including /proc and /dev.\n"
          "  Default sizes use allocated blocks and count hard-linked inodes once.\n"
          "  --apparent-size uses logical file sizes; hard links still count once.\n"
          "  --summary prints scan totals without opening a window.\n"
          "  --export CSV writes a report; use - for standard output.\n"
          "  Mouse: click a file; click a directory in the list to zoom; double-click\n"
          "         a treemap tile to zoom; Ctrl+click opens its folder in the file\n"
          "         manager; right-click goes back; wheel scrolls.\n"
          "  Keys: arrows/Home/End select; Enter opens a directory; Backspace/Left\n"
          "        go back; A changes size mode; E exports CSV; R rescans; Q quits.\n", out);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    int summary = 0;
    const char *export_path = NULL;
    ScanOptions options = {.same_filesystem = 1};
    const char *path = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }
        if (!strcmp(argv[i], "--summary")) { summary = 1; continue; }
        if (!strcmp(argv[i], "--export")) {
            if (++i >= argc) { usage(stderr); return 2; }
            export_path = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--all-filesystems")) { options.same_filesystem = 0; continue; }
        if (!strcmp(argv[i], "--apparent-size")) { options.apparent_size = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) { usage(stderr); return 2; }
        path = argv[i];
    }
    if (summary && export_path) { usage(stderr); return 2; }
    char *canonical = realpath(path, NULL);
    if (!canonical) { fprintf(stderr, "DebBarStat: %s: %s\n", path, strerror(errno)); return 1; }
    if (summary || export_path) {
        ScanStats stats;
        Node *root = scan_tree(canonical, options, &stats, NULL, NULL);
        if (!root) {
            if (errno == EOPNOTSUPP)
                fprintf(stderr, "DebBarStat: virtual filesystem excluded: %s\n", canonical);
            else fprintf(stderr, "DebBarStat: scan failed: %s\n", strerror(errno));
            free(canonical);
            return 1;
        }
        if (summary) {
            printf("Path: %s\nBytes: %" PRIu64 "\nFiles: %" PRIu64 "\nDirectories: %" PRIu64
                   "\nSymlinks: %" PRIu64 "\nOther: %" PRIu64 "\nErrors: %" PRIu64
                   "\nHard-link copies: %" PRIu64
                   "\nMounts skipped: %" PRIu64 "\nVirtual filesystems skipped: %" PRIu64
                   "\n", canonical, root->size, stats.files, stats.directories,
                   stats.symlinks, stats.other, stats.errors, stats.hardlink_duplicates,
                   stats.mount_skips, stats.virtual_skips);
        } else {
            int result = !strcmp(export_path, "-") ?
                report_write_csv(stdout, root, options, &stats) :
                report_export_csv(export_path, root, options, &stats);
            if (result == 0 && !strcmp(export_path, "-") && fflush(stdout) != 0)
                result = -1;
            if (result != 0) {
                fprintf(stderr, "DebBarStat: report export failed: %s\n", strerror(errno));
                node_free(root); free(canonical); return 1;
            }
            if (strcmp(export_path, "-")) printf("Saved report: %s\n", export_path);
        }
        node_free(root); free(canonical);
        return stats.errors ? 3 : 0;
    }
    App app = {.path = canonical, .scan_options = options};
    if (make_window(&app) != 0) { free(canonical); return 1; }
    signal(SIGCHLD, SIG_IGN);
    if (rescan(&app) != 0) {
        if (errno == EOPNOTSUPP)
            fprintf(stderr, "DebBarStat: virtual filesystem excluded: %s\n", canonical);
        else fprintf(stderr, "DebBarStat: scan failed: %s\n", strerror(errno));
        XCloseDisplay(app.display); free(canonical); return 1;
    }
    draw(&app);
    int redraw = 0, resizing = 0;
    int64_t resize_deadline = 0;
    while (!app.quit) {
        for (int batch = 0; batch < 256 && XPending(app.display); batch++) {
            XEvent event;
            XNextEvent(app.display, &event);
            if (event.type == Expose) redraw = 1;
            else if (event.type == ConfigureNotify) {
                if (app.width != event.xconfigure.width || app.height != event.xconfigure.height) {
                    app.width = event.xconfigure.width;
                    app.height = event.xconfigure.height;
                    redraw = resizing = 1;
                    /* Paint once when a resize burst settles. */
                    resize_deadline = monotonic_ms() + 160;
                }
            } else if (event.type == KeyPress) {
                if (handle_key(&app, &event.xkey)) { redraw = 1; resizing = 0; }
            } else if (event.type == ButtonPress) {
                if (redraw && resizing) { draw(&app); redraw = resizing = 0; }
                if (handle_button(&app, &event.xbutton)) { redraw = 1; resizing = 0; }
            } else if (event.type == ClientMessage &&
                       (Atom)event.xclient.data.l[0] == app.close_atom) app.quit = 1;
            else if (event.type == DestroyNotify) app.quit = 1;
            if (app.quit) break;
        }
        if (app.quit) break;
        int64_t now = monotonic_ms();
        if (redraw && (!resizing || now >= resize_deadline)) {
            draw(&app);
            redraw = resizing = 0;
            continue;
        }
        if (XPending(app.display)) continue;
        int timeout = resizing ? (int)(resize_deadline - now) : -1;
        if (timeout < 0 && resizing) timeout = 0;
        struct pollfd fd = {.fd = ConnectionNumber(app.display), .events = POLLIN};
        if (poll(&fd, 1, timeout) < 0 && errno != EINTR) {
            perror("DebBarStat: X event wait failed");
            break;
        }
    }
    node_free(app.root);
    XFreeFont(app.display, app.font);
    XFreeGC(app.display, app.gc);
    XCloseDisplay(app.display);
    free(canonical);
    return 0;
}
