#define _POSIX_C_SOURCE 200809L
#include "scan.h"

#include <dirent.h>
#include <errno.h>
#include <linux/magic.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>

typedef struct {
    dev_t device;
    ino_t inode;
    unsigned used : 1;
} InodeEntry;

typedef struct {
    InodeEntry *entries;
    size_t capacity, count;
} InodeSet;

typedef struct {
    dev_t root_device;
    ScanOptions options;
    InodeSet seen;
    int failed;
    ScanStats *stats;
    ScanProgress progress;
    void *context;
} Walk;

static uint64_t inode_size(const struct stat *st, ScanOptions options) {
    if (options.apparent_size)
        return st->st_size > 0 ? (uint64_t)st->st_size : 0;
    if (st->st_blocks <= 0) return 0;
    uint64_t blocks = (uint64_t)st->st_blocks;
    return blocks > UINT64_MAX / 512 ? UINT64_MAX : blocks * 512;
}

static Node *new_node(const char *name, Node *parent, const struct stat *st,
                      ScanOptions options) {
    Node *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->name = strdup(name);
    if (!n->name) { free(n); return NULL; }
    n->parent = parent;
    n->is_dir = S_ISDIR(st->st_mode);
    n->is_regular = S_ISREG(st->st_mode);
    n->is_symlink = S_ISLNK(st->st_mode);
    n->self_size = inode_size(st, options);
    n->size = n->self_size;
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

static uint64_t inode_hash(dev_t device, ino_t inode) {
    uint64_t x = (uint64_t)device ^ ((uint64_t)inode * UINT64_C(0x9e3779b97f4a7c15));
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static int inode_set_grow(InodeSet *set) {
    if (set->capacity > SIZE_MAX / 2) return -1;
    size_t capacity = set->capacity ? set->capacity * 2 : 1024;
    InodeEntry *next = calloc(capacity, sizeof(*next));
    if (!next) return -1;
    for (size_t i = 0; i < set->capacity; i++) {
        InodeEntry entry = set->entries[i];
        if (!entry.used) continue;
        size_t index = (size_t)inode_hash(entry.device, entry.inode) & (capacity - 1);
        while (next[index].used) index = (index + 1) & (capacity - 1);
        next[index] = entry;
    }
    free(set->entries);
    set->entries = next;
    set->capacity = capacity;
    return 0;
}

/* Returns 1 for an already counted inode, 0 for a new one, -1 on OOM. */
static int inode_seen(InodeSet *set, dev_t device, ino_t inode) {
    if (!set->capacity || set->count >= set->capacity - set->capacity / 4)
        if (inode_set_grow(set) != 0) return -1;
    size_t index = (size_t)inode_hash(device, inode) & (set->capacity - 1);
    while (set->entries[index].used) {
        if (set->entries[index].device == device && set->entries[index].inode == inode)
            return 1;
        index = (index + 1) & (set->capacity - 1);
    }
    set->entries[index] = (InodeEntry){.device = device, .inode = inode, .used = 1};
    set->count++;
    return 0;
}

/* Virtual filesystems describe kernel state, not disk content. In particular,
 * procfs can report enormous synthetic sizes for files such as /proc/kcore. */
static int is_virtual_filesystem(long type) {
    switch ((unsigned long)type) {
        case PROC_SUPER_MAGIC:
        case SYSFS_MAGIC:
        case TMPFS_MAGIC:
        case RAMFS_MAGIC:
        case DEVPTS_SUPER_MAGIC:
        case CGROUP_SUPER_MAGIC:
        case CGROUP2_SUPER_MAGIC:
        case DEBUGFS_MAGIC:
        case TRACEFS_MAGIC:
        case SECURITYFS_MAGIC:
        case BPF_FS_MAGIC:
        case HUGETLBFS_MAGIC:
        case PSTOREFS_MAGIC:
        case EFIVARFS_MAGIC:
        case BINFMTFS_MAGIC:
        case BINDERFS_SUPER_MAGIC:
        case AUTOFS_SUPER_MAGIC:
            return 1;
        default:
            return 0;
    }
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

static int walk_dir(Node *parent, const char *path, dev_t device,
                    Walk *walk, unsigned depth) {
    if (depth >= 1024) { walk->stats->errors++; return 0; }
    DIR *dir = opendir(path);
    if (!dir) { walk->stats->errors++; return 0; }
    walk->stats->directories++;
    if (walk->progress && (walk->stats->directories % 32 == 1))
        walk->progress(walk->stats, path, walk->context);
    struct dirent *ent;
    while (!walk->stats->cancelled && !walk->failed) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            if (errno) walk->stats->errors++;
            break;
        }
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char *child_path = join_path(path, ent->d_name);
        if (!child_path) { walk->stats->errors++; continue; }
        struct stat st;
        if (lstat(child_path, &st) != 0) {
            walk->stats->errors++;
            free(child_path);
            continue;
        }
        if (st.st_dev != device) {
            struct statfs fs;
            if (statfs(child_path, &fs) != 0) {
                walk->stats->errors++;
                free(child_path);
                continue;
            }
            if (is_virtual_filesystem(fs.f_type)) {
                walk->stats->virtual_skips++;
                free(child_path);
                continue;
            }
            if (walk->options.same_filesystem && st.st_dev != walk->root_device) {
                walk->stats->mount_skips++;
                free(child_path);
                continue;
            }
        }
        Node *child = new_node(ent->d_name, parent, &st, walk->options);
        if (!child || add_child(parent, child) != 0) {
            node_free(child);
            walk->stats->errors++;
            free(child_path);
            continue;
        }
        if (!child->is_dir && st.st_nlink > 1) {
            int seen = inode_seen(&walk->seen, st.st_dev, st.st_ino);
            if (seen < 0) { walk->failed = 1; free(child_path); break; }
            if (seen) {
                child->size = child->self_size = 0;
                child->is_hardlink_duplicate = 1;
                walk->stats->hardlink_duplicates++;
            }
        }
        if (child->is_dir) {
            walk_dir(child, child_path, st.st_dev, walk, depth + 1);
        } else if (child->is_symlink) {
            walk->stats->symlinks++;
        } else if (S_ISREG(st.st_mode)) {
            walk->stats->files++;
        } else {
            walk->stats->other++;
        }
        parent->size = add_size(parent->size, child->size);
        free(child_path);
        if (walk->progress && ((walk->stats->files + walk->stats->symlinks +
                                walk->stats->other) % 2048 == 0))
            walk->progress(walk->stats, path, walk->context);
    }
    closedir(dir);
    return 0;
}

Node *scan_tree(const char *path, ScanOptions options, ScanStats *stats,
                ScanProgress progress, void *context) {
    if (!path || !stats) { errno = EINVAL; return NULL; }
    memset(stats, 0, sizeof(*stats));
    struct stat st;
    if (lstat(path, &st) != 0) return NULL;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return NULL; }
    struct statfs fs;
    if (statfs(path, &fs) != 0) return NULL;
    if (is_virtual_filesystem(fs.f_type)) { errno = EOPNOTSUPP; return NULL; }
    Node *root = new_node(path, NULL, &st, options);
    if (!root) return NULL;
    Walk walk = {.root_device = st.st_dev, .options = options,
                 .stats = stats, .progress = progress, .context = context};
    walk_dir(root, path, st.st_dev, &walk, 0);
    free(walk.seen.entries);
    if (walk.failed) { node_free(root); errno = ENOMEM; return NULL; }
    stats->bytes = root->size;
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
