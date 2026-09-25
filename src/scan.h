#ifndef DEBBARSTAT_SCAN_H
#define DEBBARSTAT_SCAN_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct Node {
    char *name;
    struct Node *parent;
    struct Node **children;
    size_t child_count, child_capacity;
    uint64_t size;
    unsigned is_dir : 1;
    unsigned is_symlink : 1;
    int x, y, w, h;
} Node;

typedef struct {
    uint64_t files, directories, symlinks, errors, other, bytes;
    uint64_t mount_skips, virtual_skips;
    int cancelled;
} ScanStats;

typedef void (*ScanProgress)(ScanStats *stats, const char *path, void *context);

Node *scan_tree(const char *path, int same_filesystem, ScanStats *stats,
                ScanProgress progress, void *context);
void node_free(Node *node);
char *node_path(const Node *node);
void node_sort(Node *node);

#endif
