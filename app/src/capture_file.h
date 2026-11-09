#ifndef SC_CAPTURE_FILE_H
#define SC_CAPTURE_FILE_H
#include <stdint.h>
#include <stdio.h>

// Atomically reserve a timestamped .mp4 or .png file with mode 0600.
// Caller owns the FILE and allocated path. Existing files are never overwritten.
FILE *sc_capture_create_file(const char *directory, int64_t unix_ms,
                            const char *extension, char **path);
#endif
