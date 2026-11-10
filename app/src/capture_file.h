#ifndef SC_CAPTURE_FILE_H
#define SC_CAPTURE_FILE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Atomically reserve a timestamped .mp4 or .png file with mode 0600.
// Caller owns the FILE and allocated path. Existing files are never overwritten.
FILE *sc_capture_create_file(const char *directory, int64_t unix_ms,
                            const char *extension, char **path);

// Atomically publish a completed temporary file at a collision-safe timestamped
// name using exclusive rename (macOS) or a hard link. No existing file is
// replaced. Caller owns path on success and must remove any remaining temporary
// file on success or failure (exclusive rename already removes that name).
bool sc_capture_publish_file(const char *temporary, const char *directory,
                             int64_t unix_ms, const char *extension, char **path);
#endif
