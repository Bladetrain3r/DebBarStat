#define _POSIX_C_SOURCE 200809L
#include "report.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *text;
    size_t length, capacity;
} PathBuffer;

static int append(PathBuffer *path, const char *part, int separator) {
    size_t length = strlen(part);
    size_t extra = (size_t)separator;
    if (path->length > SIZE_MAX - length - extra - 1) { errno = ENOMEM; return -1; }
    size_t needed = path->length + length + extra + 1;
    if (needed > path->capacity) {
        size_t capacity = path->capacity ? path->capacity : 256;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { errno = ENOMEM; return -1; }
            capacity *= 2;
        }
        char *next = realloc(path->text, capacity);
        if (!next) return -1;
        path->text = next;
        path->capacity = capacity;
    }
    if (separator) path->text[path->length++] = '/';
    memcpy(path->text + path->length, part, length + 1);
    path->length += length;
    return 0;
}

static void csv_string(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"') fputc('"', out);
        fputc(*p, out);
    }
    fputc('"', out);
}

static int write_node(FILE *out, const Node *node, PathBuffer *path,
                      const char *mode, const ScanStats *stats, unsigned depth) {
    if (depth > 1024) { errno = EOVERFLOW; return -1; }
    csv_string(out, path->text);
    const char *kind = node->is_dir ? "directory" : node->is_symlink ? "symlink" :
                       node->is_regular ? "file" : "other";
    if (fprintf(out, ",%s,%" PRIu64 ",%" PRIu64 ",%u,%s,%" PRIu64 "\n",
                kind, node->self_size, node->size,
                (unsigned)node->is_hardlink_duplicate, mode, stats->errors) < 0) return -1;
    for (size_t i = 0; i < node->child_count; i++) {
        size_t previous = path->length;
        if (append(path, node->children[i]->name,
                   path->length && path->text[path->length - 1] != '/') != 0) return -1;
        if (write_node(out, node->children[i], path, mode, stats, depth + 1) != 0) return -1;
        path->length = previous;
        path->text[previous] = '\0';
    }
    return ferror(out) ? -1 : 0;
}

int report_write_csv(FILE *out, const Node *root, ScanOptions options,
                     const ScanStats *stats) {
    if (!out || !root || !stats) { errno = EINVAL; return -1; }
    if (fputs("path,kind,self_bytes,total_bytes,hardlink_duplicate,size_mode,scan_errors\n",
              out) == EOF) return -1;
    PathBuffer path = {0};
    int result = append(&path, root->name, 0);
    if (result == 0)
        result = write_node(out, root, &path,
                            options.apparent_size ? "apparent" : "allocated", stats, 0);
    free(path.text);
    return result;
}

int report_export_csv(const char *path, const Node *root, ScanOptions options,
                      const ScanStats *stats) {
    if (!path) { errno = EINVAL; return -1; }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    FILE *out = fdopen(fd, "w");
    if (!out) { int saved = errno; close(fd); unlink(path); errno = saved; return -1; }
    errno = 0;
    int result = report_write_csv(out, root, options, stats);
    int saved = errno;
    if (fclose(out) != 0 && result == 0) { result = -1; saved = errno; }
    if (result != 0) { unlink(path); errno = saved ? saved : EIO; }
    return result;
}
