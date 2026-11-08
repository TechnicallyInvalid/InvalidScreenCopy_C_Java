#include "recording.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    setenv("TZ", "UTC", 1);
    tzset();
    char directory[] = "/tmp/invalid-recording-name-XXXXXX";
    assert(mkdtemp(directory));
    char *first, *second, *third;
    FILE *file = sc_recording_create_file(directory, 1794100000123, &first);
    assert(file);
    fputs("existing data", file);
    fclose(file);
    assert(strstr(first, "2026-11-08-01-06-40-123.mp4"));
    file = sc_recording_create_file(directory, 1794100000123, &second);
    assert(file);
    fclose(file);
    assert(strstr(second, "-123-001.mp4"));
    char *link;
    assert(asprintf(&link, "%s/2026-11-08-01-06-40-123-002.mp4", directory) >= 0);
    assert(!symlink(first, link));
    file = sc_recording_create_file(directory, 1794100000123, &third);
    assert(file);
    fclose(file);
    assert(strstr(third, "-123-003.mp4"));
    struct stat info;
    assert(!stat(first, &info) && info.st_size == 13);
    assert((info.st_mode & 0777) == 0600);
    char *failed;
    assert(!sc_recording_create_file("/no/such/recording/directory", 1794100000123, &failed));
    assert(errno == ENOENT && !failed);
    unlink(first); unlink(second); unlink(third); unlink(link); rmdir(directory);
    free(first); free(second); free(third); free(link);
    return 0;
}
