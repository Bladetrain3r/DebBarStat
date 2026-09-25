#ifndef DEBBARSTAT_REPORT_H
#define DEBBARSTAT_REPORT_H

#include "scan.h"

#include <stdio.h>

/* CSV rows contain a node's own bytes and its inclusive subtree total. */
int report_write_csv(FILE *out, const Node *root, ScanOptions options,
                     const ScanStats *stats);

/* Creates a new file and refuses to overwrite an existing report. */
int report_export_csv(const char *path, const Node *root, ScanOptions options,
                      const ScanStats *stats);

#endif
