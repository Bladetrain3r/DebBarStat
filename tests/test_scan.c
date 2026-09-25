#define _POSIX_C_SOURCE 200809L
#include "scan.h"

#include <assert.h>
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

int main(void) {
    char dir[] = "/tmp/debbarstat-test-XXXXXX";
    assert(mkdtemp(dir));
    char path[512];
    snprintf(path, sizeof(path), "%s/sub", dir);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/a.txt", dir);
    write_file(path, "abc");
    snprintf(path, sizeof(path), "%s/sub/b.bin", dir);
    write_file(path, "12345");
    snprintf(path, sizeof(path), "%s/loop", dir);
    assert(symlink(".", path) == 0);
    ScanStats stats;
    Node *root = scan_tree(dir, 1, &stats, NULL, NULL);
    assert(root);
    assert(root->size == 8);
    assert(stats.files == 2 && stats.directories == 2 && stats.symlinks == 1);
    assert(stats.errors == 0 && stats.bytes == 8);
    assert(root->child_count == 3);
    assert(root->children[0]->size == 5);
    char *actual = node_path(root->children[0]);
    snprintf(path, sizeof(path), "%s/sub", dir);
    assert(strcmp(actual, path) == 0);
    free(actual);
    node_free(root);
    snprintf(path, sizeof(path), "%s/loop", dir); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/a.txt", dir); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/sub/b.bin", dir); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/sub", dir); assert(rmdir(path) == 0);
    assert(rmdir(dir) == 0);
    puts("scanner tests passed");
    return 0;
}
