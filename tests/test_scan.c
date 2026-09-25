#define _POSIX_C_SOURCE 200809L
#include "scan.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *data) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, data, strlen(data)) == (ssize_t)strlen(data));
    assert(close(fd) == 0);
}

static Node *child_named(Node *parent, const char *name) {
    for (size_t i = 0; i < parent->child_count; i++)
        if (!strcmp(parent->children[i]->name, name)) return parent->children[i];
    return NULL;
}

static uint64_t stat_bytes(const char *path, int apparent) {
    struct stat st;
    assert(lstat(path, &st) == 0);
    return apparent ? (uint64_t)st.st_size : (uint64_t)st.st_blocks * 512;
}

static void check_mode(const char *dir, int apparent) {
    char path[512];
    ScanStats stats;
    ScanOptions options = {.same_filesystem = 1, .apparent_size = apparent};
    Node *root = scan_tree(dir, options, &stats, NULL, NULL);
    assert(root);
    assert(stats.files == 4 && stats.directories == 2 && stats.symlinks == 1);
    assert(stats.hardlink_duplicates == 1 && stats.errors == 0);
    Node *sub = child_named(root, "sub");
    Node *a = child_named(root, "a.txt");
    Node *other_link = child_named(sub, "a-hardlink");
    assert(sub && a && other_link);
    assert(a->is_hardlink_duplicate != other_link->is_hardlink_duplicate);
    snprintf(path, sizeof(path), "%s/a.txt", dir);
    assert(a->size + other_link->size == stat_bytes(path, apparent));

    uint64_t expected = stat_bytes(dir, apparent);
    const char *unique[] = {"sub", "a.txt", "sub/b.bin", "loop", "sparse.bin"};
    for (size_t i = 0; i < sizeof(unique) / sizeof(unique[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, unique[i]);
        expected += stat_bytes(path, apparent);
    }
    assert(root->size == expected && stats.bytes == expected);
    char *actual = node_path(sub);
    snprintf(path, sizeof(path), "%s/sub", dir);
    assert(strcmp(actual, path) == 0);
    free(actual);
    node_free(root);
}

int main(void) {
    assert(sizeof(off_t) >= 8);
    char dir[] = "./build/debbarstat-test-XXXXXX";
    assert(mkdtemp(dir));
    char path[512], source[512];
    snprintf(path, sizeof(path), "%s/sub", dir);
    assert(mkdir(path, 0700) == 0);
    snprintf(source, sizeof(source), "%s/a.txt", dir);
    write_file(source, "abc");
    snprintf(path, sizeof(path), "%s/sub/a-hardlink", dir);
    assert(link(source, path) == 0);
    snprintf(path, sizeof(path), "%s/sub/b.bin", dir);
    write_file(path, "12345");
    snprintf(path, sizeof(path), "%s/loop", dir);
    assert(symlink(".", path) == 0);
    snprintf(path, sizeof(path), "%s/sparse.bin", dir);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)(UINT64_C(5) * 1024 * 1024 * 1024)) == 0);
    assert(close(fd) == 0);

    check_mode(dir, 0);
    check_mode(dir, 1);

    snprintf(path, sizeof(path), "%s/sparse.bin", dir); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/loop", dir); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/sub/a-hardlink", dir); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/a.txt", dir); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/sub/b.bin", dir); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/sub", dir); assert(rmdir(path) == 0);
    assert(rmdir(dir) == 0);

    ScanStats stats;
    ScanOptions options = {.same_filesystem = 0};
    assert(scan_tree("/proc", options, &stats, NULL, NULL) == NULL);
    assert(errno == EOPNOTSUPP);
    assert(scan_tree("/dev", options, &stats, NULL, NULL) == NULL);
    assert(errno == EOPNOTSUPP);
    puts("scanner tests passed");
    return 0;
}
