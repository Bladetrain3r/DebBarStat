#define _POSIX_C_SOURCE 200809L
#include "scan.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
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
    Time last_click;
    Node *last_hit;
} App;

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

static void clear_rects(Node *n) {
    n->w = n->h = 0;
    for (size_t i = 0; i < n->child_count; i++) clear_rects(n->children[i]);
}

static void layout_node(App *app, Node *node, Rect r, unsigned depth);

static void layout_group(App *app, Node **items, size_t lo, size_t hi, Rect r,
                         uint64_t total, unsigned depth) {
    if (lo >= hi || r.w < 1 || r.h < 1 || total == 0) return;
    if (hi - lo == 1) { layout_node(app, items[lo], r, depth); return; }
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
    if (r.w < 1 || r.h < 1) return;
    if (!node->is_dir || node->child_count == 0 || depth > 1000) {
        fill(app, hash_type(node), r);
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

static Node *hit(Node *node, int x, int y) {
    if (!node || x < node->x || y < node->y || x >= node->x + node->w ||
        y >= node->y + node->h || node->w <= 0 || node->h <= 0) return NULL;
    for (size_t i = 0; i < node->child_count; i++) {
        Node *found = hit(node->children[i], x, y);
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
          "DebBarStat  |  A size mode  |  R rescan  |  Backspace back");
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
    clear_rects(app->focus);
    if (app->focus->size && map.w > 0 && map.h > 0) layout_node(app, app->focus, map, 0);
    else label(app, 4, map.x + 12, map.y + 24, map.w - 24, "No nonempty files");
    if (app->selected && app->selected->w > 3 && app->selected->h > 3)
        line(app, 5, app->selected->x, app->selected->y, app->selected->w, app->selected->h);
    fill(app, 1, (Rect){0, app->height - BOTTOM, app->width, BOTTOM});
    const Node *detail = app->selected ? app->selected : app->focus;
    char *detail_path = node_path(detail);
    format_size(detail->size, size, sizeof(size));
    snprintf(linebuf, sizeof(linebuf), "%s%s  |  %s", size,
             detail->is_hardlink_duplicate ? " (hard link counted elsewhere)" : "",
             detail_path ? detail_path : "");
    label(app, 4, PAD, app->height - 12, app->width - 2 * PAD, linebuf);
    free(detail_path);
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
    draw(app);
}

static int rescan(App *app) {
    Node *previous_focus = app->focus;
    Node *previous_selection = app->selected;
    int previous_scroll = app->scroll;
    app->scanning = 1;
    app->selected = app->focus = NULL;
    app->scroll = 0;
    app->last_hit = NULL;
    snprintf(app->progress_path, sizeof(app->progress_path), "%s", app->path);
    draw(app);
    ScanStats stats;
    Node *next = scan_tree(app->path, app->scan_options, &stats, progress, app);
    app->stats = stats;
    app->scanning = 0;
    if (!next) {
        app->focus = previous_focus;
        app->selected = previous_selection;
        app->scroll = previous_scroll;
        draw(app);
        return -1;
    }
    node_free(app->root);
    app->root = app->focus = next;
    draw(app);
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

static void handle_key(App *app, XKeyEvent *event) {
    KeySym key = XLookupKeysym(event, 0);
    if (key == XK_q || key == XK_Escape) app->quit = 1;
    else if (key == XK_BackSpace || key == XK_Left) {
        if (app->focus->parent) app->focus = app->focus->parent;
        app->selected = NULL; app->scroll = 0;
    } else if (key == XK_r || key == XK_R) {
        if (rescan(app) != 0) fprintf(stderr, "DebBarStat: rescan failed: %s\n", strerror(errno));
    } else if (key == XK_a || key == XK_A) {
        app->scan_options.apparent_size = !app->scan_options.apparent_size;
        if (rescan(app) != 0) {
            app->scan_options.apparent_size = !app->scan_options.apparent_size;
            fprintf(stderr, "DebBarStat: size-mode scan failed: %s\n", strerror(errno));
        }
    } else if ((key == XK_Down || key == XK_Up || key == XK_Home || key == XK_End) &&
               app->focus->child_count) {
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
    }
    else if (key == XK_Return && app->selected && app->selected->is_dir) {
        app->focus = app->selected; app->selected = NULL; app->scroll = 0;
    }
}

static void handle_button(App *app, XButtonEvent *event) {
    if (event->button == Button3) {
        if (app->focus->parent) app->focus = app->focus->parent;
        app->selected = NULL; app->scroll = 0;
        return;
    }
    if (event->x < SIDE && event->y >= TOP + 77) {
        if (event->button == Button4 && app->scroll > 0) app->scroll--;
        if (event->button == Button5 && (size_t)(app->scroll + 1) < app->focus->child_count)
            app->scroll++;
        if (event->button != Button1) return;
        int index = (event->y - (TOP + 79)) / ROW + app->scroll;
        if (index >= 0 && (size_t)index < app->focus->child_count) {
            Node *n = app->focus->children[index];
            if (n->is_dir) { app->focus = n; app->selected = NULL; app->scroll = 0; }
            else app->selected = n;
        }
        return;
    }
    if (event->button != Button1 || event->x < SIDE || event->y < TOP) return;
    Node *n = hit(app->focus, event->x, event->y);
    if (!n) return;
    if (n == app->last_hit && event->time - app->last_click < 350) {
        Node *dir = n->is_dir ? n : n->parent;
        if (dir && dir != app->focus) { app->focus = dir; app->scroll = 0; }
        app->selected = NULL;
    } else app->selected = n;
    app->last_hit = n;
    app->last_click = event->time;
}

static void usage(FILE *out) {
    fputs("Usage: debbarstat [--all-filesystems] [--apparent-size] [--summary] [PATH]\n"
          "  PATH defaults to the current directory. Symlinks are never followed.\n"
          "  By default, mounted filesystems beneath PATH are skipped.\n"
          "  Virtual filesystems are always skipped, including /proc and /dev.\n"
          "  Default sizes use allocated blocks and count hard-linked inodes once.\n"
          "  --apparent-size uses logical file sizes; hard links still count once.\n"
          "  --summary prints scan totals without opening a window.\n"
          "  Mouse: click a file; click a directory in the list to zoom; double-click\n"
          "         a treemap tile to zoom; right-click to go back; wheel to scroll.\n"
          "  Keys: arrows/Home/End select; Enter opens a directory; Backspace/Left\n"
          "        go back; A changes size mode; R rescans; Q/Escape quits.\n", out);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    int summary = 0;
    ScanOptions options = {.same_filesystem = 1};
    const char *path = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }
        if (!strcmp(argv[i], "--summary")) { summary = 1; continue; }
        if (!strcmp(argv[i], "--all-filesystems")) { options.same_filesystem = 0; continue; }
        if (!strcmp(argv[i], "--apparent-size")) { options.apparent_size = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) { usage(stderr); return 2; }
        path = argv[i];
    }
    char *canonical = realpath(path, NULL);
    if (!canonical) { fprintf(stderr, "DebBarStat: %s: %s\n", path, strerror(errno)); return 1; }
    if (summary) {
        ScanStats stats;
        Node *root = scan_tree(canonical, options, &stats, NULL, NULL);
        if (!root) {
            if (errno == EOPNOTSUPP)
                fprintf(stderr, "DebBarStat: virtual filesystem excluded: %s\n", canonical);
            else fprintf(stderr, "DebBarStat: scan failed: %s\n", strerror(errno));
            free(canonical);
            return 1;
        }
        printf("Path: %s\nBytes: %" PRIu64 "\nFiles: %" PRIu64 "\nDirectories: %" PRIu64
               "\nSymlinks: %" PRIu64 "\nOther: %" PRIu64 "\nErrors: %" PRIu64
               "\nHard-link copies: %" PRIu64
               "\nMounts skipped: %" PRIu64 "\nVirtual filesystems skipped: %" PRIu64
               "\n", canonical, root->size, stats.files, stats.directories,
               stats.symlinks, stats.other, stats.errors, stats.hardlink_duplicates,
               stats.mount_skips,
               stats.virtual_skips);
        node_free(root); free(canonical);
        return stats.errors ? 3 : 0;
    }
    App app = {.path = canonical, .scan_options = options};
    if (make_window(&app) != 0) { free(canonical); return 1; }
    if (rescan(&app) != 0) {
        if (errno == EOPNOTSUPP)
            fprintf(stderr, "DebBarStat: virtual filesystem excluded: %s\n", canonical);
        else fprintf(stderr, "DebBarStat: scan failed: %s\n", strerror(errno));
        XCloseDisplay(app.display); free(canonical); return 1;
    }
    while (!app.quit) {
        XEvent event;
        XNextEvent(app.display, &event);
        if (event.type == Expose && event.xexpose.count == 0) draw(&app);
        else if (event.type == ConfigureNotify) {
            app.width = event.xconfigure.width; app.height = event.xconfigure.height;
            draw(&app);
        } else if (event.type == KeyPress) { handle_key(&app, &event.xkey); draw(&app); }
        else if (event.type == ButtonPress) { handle_button(&app, &event.xbutton); draw(&app); }
        else if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == app.close_atom)
            app.quit = 1;
        else if (event.type == DestroyNotify) app.quit = 1;
    }
    node_free(app.root);
    XFreeFont(app.display, app.font);
    XFreeGC(app.display, app.gc);
    XCloseDisplay(app.display);
    free(canonical);
    return 0;
}
