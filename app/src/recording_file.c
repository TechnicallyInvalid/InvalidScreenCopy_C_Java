#include "recording.h"
#include "capture_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/stdio.h>
#endif

static bool
sc_capture_format_stamp(int64_t unix_ms, const char *extension,
                         char *stamp, size_t stamp_size) {
    if (!extension || (strcmp(extension, "mp4") && strcmp(extension, "png"))) {
        errno = EINVAL;
        return false;
    }
    time_t seconds = unix_ms / 1000;
    struct tm local;
    if (unix_ms < 0 || !localtime_r(&seconds, &local)) {
        errno = EINVAL;
        return false;
    }
    if (!strftime(stamp, stamp_size, "%Y-%m-%d-%H-%M-%S", &local)) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static char *
sc_capture_candidate(const char *directory, const char *stamp, int64_t unix_ms,
                      const char *extension, unsigned collision) {
    char suffix[24] = "";
    if (collision) {
        snprintf(suffix, sizeof(suffix), "-%03u", collision);
    }
    char *candidate;
    if (asprintf(&candidate, "%s/%s-%03d%s.%s", directory, stamp,
                 (int) (unix_ms % 1000), suffix, extension) < 0) {
        return NULL;
    }
    return candidate;
}

FILE *
sc_capture_create_file(const char *directory, int64_t unix_ms,
                       const char *extension, char **path) {
    *path = NULL;
    char stamp[32];
    if (!sc_capture_format_stamp(unix_ms, extension, stamp, sizeof(stamp))) {
        return NULL;
    }
    for (unsigned collision = 0; collision < 10000; ++collision) {
        char *candidate = sc_capture_candidate(directory, stamp, unix_ms,
                                                extension, collision);
        if (!candidate) {
            return NULL;
        }
        int fd = open(candidate, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            FILE *file = fdopen(fd, "wb");
            if (!file) {
                int error = errno;
                close(fd);
                unlink(candidate);
                free(candidate);
                errno = error;
                return NULL;
            }
            *path = candidate;
            return file;
        }
        int error = errno;
        free(candidate);
        if (error != EEXIST) {
            errno = error;
            return NULL;
        }
    }
    errno = EEXIST;
    return NULL;
}

bool
sc_capture_publish_file(const char *temporary, const char *directory,
                         int64_t unix_ms, const char *extension, char **path) {
    *path = NULL;
    char stamp[32];
    if (!sc_capture_format_stamp(unix_ms, extension, stamp, sizeof(stamp))) {
        return false;
    }
    for (unsigned collision = 0; collision < 10000; ++collision) {
        char *candidate = sc_capture_candidate(directory, stamp, unix_ms,
                                                extension, collision);
        if (!candidate) {
            return false;
        }
        // Exclusive rename also works on filesystems without hard links.
        // Both publication methods fail on an existing destination, including
        // symlinks. Never use ordinary rename(), which could overwrite a file.
#ifdef __APPLE__
        int result = renamex_np(temporary, candidate, RENAME_EXCL);
        if (result && (errno == ENOTSUP || errno == EOPNOTSUPP)) {
            result = link(temporary, candidate);
        }
#else
        int result = link(temporary, candidate);
#endif
        if (!result) {
            *path = candidate;
            return true;
        }
        int error = errno;
        free(candidate);
        if (error != EEXIST) {
            errno = error;
            return false;
        }
    }
    errno = EEXIST;
    return false;
}

FILE *
sc_recording_create_file(const char *directory, int64_t unix_ms, char **path) {
    return sc_capture_create_file(directory, unix_ms, "mp4", path);
}
