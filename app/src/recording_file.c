#include "recording.h"
#include "capture_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

FILE *
sc_capture_create_file(const char *directory, int64_t unix_ms,
                       const char *extension, char **path) {
    *path = NULL;
    if (!extension || (strcmp(extension, "mp4") && strcmp(extension, "png"))) {
        errno = EINVAL;
        return NULL;
    }
    time_t seconds = unix_ms / 1000;
    struct tm local;
    if (unix_ms < 0 || !localtime_r(&seconds, &local)) {
        errno = EINVAL;
        return NULL;
    }
    char stamp[32];
    if (!strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H-%M-%S", &local)) {
        errno = EINVAL;
        return NULL;
    }
    for (unsigned collision = 0; collision < 10000; ++collision) {
        char suffix[24] = "";
        if (collision) {
            snprintf(suffix, sizeof(suffix), "-%03u", collision);
        }
        char *candidate;
        if (asprintf(&candidate, "%s/%s-%03d%s.%s", directory, stamp,
                     (int) (unix_ms % 1000), suffix, extension) < 0) {
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

FILE *
sc_recording_create_file(const char *directory, int64_t unix_ms, char **path) {
    return sc_capture_create_file(directory, unix_ms, "mp4", path);
}
