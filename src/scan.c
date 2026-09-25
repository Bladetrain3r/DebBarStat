#define _POSIX_C_SOURCE 200809L
#include "scan.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    dev_t root_device;
    int same_filesystem;
    ScanStats *stats;
    ScanProgress progress;
    void *context;
} Walk;

static Node *new_node(const char *name, Node *parent, const struct stat *st) {
    Node *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->name = strdup(name);
    if (!n->name) { free(n); return NULL; }
    n->parent = parent;
    n->is_dir = S_ISDIR(st->st_mode);
    n->is_symlink = S_ISLNK(st->st_mode);
    return n;
}

static int add_child(Node *parent, Node *child) {
    if (parent->child_count == parent->child_capacity) {
        size_t cap = parent->child_capacity ? parent->child_capacity * 2 : 16;
        if (cap < parent->child_capacity || cap > SIZE_MAX / sizeof(Node *)) return -1;
        Node **next = realloc(parent->children, cap * sizeof(Node *));
        if (!next) return -1;
        parent->children = next;
        parent->child_capacity = cap;
    }
    parent->children[parent->child_count++] = child;
    return 0;
}

static char *join_path(const char *parent, const char *name) {
    size_t a = strlen(parent), b = strlen(name);
    if (a > SIZE_MAX - b - 2) return NULL;
    char *s = malloc(a + b + 2);
    if (!s) return NULL;
    memcpy(s, parent, a);
    size_t pos = a;
    if (pos == 0 || s[pos - 1] != '/') s[pos++] = '/';
    memcpy(s + pos, name, b + 1);
    return s;
}

static uint64_t add_size(uint64_t a, uint64_t b) {
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static int cmp_nodes(const void *a, const void *b) {
    const Node *left = *(Node *const *)a, *right = *(Node *const *)b;
    if (left->size != right->size) return left->size > right->size ? -1 : 1;
    return strcmp(left->name, right->name);
}

void node_sort(Node *node) {
    if (!node) return;
    if (node->child_count > 1)
        qsort(node->children, node->child_count, sizeof(Node *), cmp_nodes);
    for (size_t i = 0; i < node->child_count; i++) node_sort(node->children[i]);
}

static int walk_dir(Node *parent, const char *path, Walk *walk, unsigned depth) {
    if (depth >= 1024) { walk->stats->errors++; return 0; }
    DIR *dir = opendir(path);
    if (!dir) { walk->stats->errors++; return 0; }
    walk->stats->directories++;
    if (walk->progress && (walk->stats->directories % 32 == 1))
        walk->progress(walk->stats, path, walk->context);
    errno = 0;
    struct dirent *ent;
    while (!walk->stats->cancelled && (ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char *child_path = join_path(path, ent->d_name);
        if (!child_path) { walk->stats->errors++; continue; }
        struct stat st;
        if (lstat(child_path, &st) != 0) {
            walk->stats->errors++;
            free(child_path);
            continue;
        }
        if (walk->same_filesystem && st.st_dev != walk->root_device) {
            walk->stats->mount_skips++;
            free(child_path);
            continue;
        }
        Node *child = new_node(ent->d_name, parent, &st);
        if (!child || add_child(parent, child) != 0) {
            node_free(child);
            walk->stats->errors++;
            free(child_path);
            continue;
        }
        if (child->is_dir) {
            walk_dir(child, child_path, walk, depth + 1);
        } else if (child->is_symlink) {
            walk->stats->symlinks++;
        } else if (S_ISREG(st.st_mode)) {
            child->size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
            walk->stats->files++;
            walk->stats->bytes = add_size(walk->stats->bytes, child->size);
        } else {
            walk->stats->other++;
        }
        parent->size = add_size(parent->size, child->size);
        free(child_path);
        if (walk->progress && ((walk->stats->files + walk->stats->symlinks +
                                walk->stats->other) % 2048 == 0))
            walk->progress(walk->stats, path, walk->context);
        errno = 0;
    }
    if (errno) walk->stats->errors++;
    closedir(dir);
    return 0;
}

Node *scan_tree(const char *path, int same_filesystem, ScanStats *stats,
                ScanProgress progress, void *context) {
    if (!path || !stats) { errno = EINVAL; return NULL; }
    memset(stats, 0, sizeof(*stats));
    struct stat st;
    if (lstat(path, &st) != 0) return NULL;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return NULL; }
    Node *root = new_node(path, NULL, &st);
    if (!root) return NULL;
    Walk walk = {.root_device = st.st_dev, .same_filesystem = same_filesystem,
                 .stats = stats, .progress = progress, .context = context};
    walk_dir(root, path, &walk, 0);
    node_sort(root);
    return root;
}

void node_free(Node *node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; i++) node_free(node->children[i]);
    free(node->children);
    free(node->name);
    free(node);
}

char *node_path(const Node *node) {
    if (!node) return NULL;
    if (!node->parent) return strdup(node->name);
    char *parent = node_path(node->parent);
    if (!parent) return NULL;
    char *path = join_path(parent, node->name);
    free(parent);
    return path;
}
