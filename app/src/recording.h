#ifndef SC_RECORDING_H
#define SC_RECORDING_H

#include "recorder.h"

struct sc_recording;
struct sc_recording_request;
struct sc_recording_input {
    struct sc_packet_sink sink;
    struct sc_recording *owner;
    AVCodecParameters *params;
    AVPacket *config;
    bool video;
    bool ready;
};

struct sc_recording {
    struct sc_recording_input video;
    struct sc_recording_input audio;
    sc_mutex mutex;
    sc_cond cond;
    sc_thread worker;
    struct sc_recording_request *request;
    uint64_t generation;
    bool worker_stopped;
    bool busy;
    bool starting;
    bool stop_requested;
    bool keyframe_requested;
    struct sc_recorder *active;
    bool audio_expected;
    bool accepting;
    bool completed;
    bool success;
    bool has_frame;
    int64_t first_pts;
    int64_t first_frame_time;
    int64_t requested_ms;
    enum sc_orientation orientation;
    char message[1024];
};

// Creates a file atomically, mode 0600. Caller owns file and allocated path.
// Collision suffixes preserve the original timestamp and never truncate files.
FILE *sc_recording_create_file(const char *directory, int64_t unix_ms,
                               char **path);
bool sc_recording_init(struct sc_recording *recording, bool audio,
                        enum sc_orientation orientation);
// UI calls only request/poll functions; all disk I/O runs on the lifecycle worker.
// The timestamp is captured at the button press, before asynchronous file creation.
bool sc_recording_request_start(struct sc_recording *recording,
                                 const char *directory, int64_t unix_ms);
void sc_recording_request_stop(struct sc_recording *recording);
// Starting may be cancelled; a writer already finalizing cannot be stopped again.
bool sc_recording_can_stop(struct sc_recording *recording);
// Consume once from the UI timer after file creation and sink setup complete.
bool sc_recording_take_keyframe_request(struct sc_recording *recording);
// Blocking variants for non-UI callers and final shutdown after streams stop.
bool sc_recording_start(struct sc_recording *recording, const char *directory,
                         int64_t unix_ms);
void sc_recording_stop(struct sc_recording *recording);
// Poll never joins a worker or performs file I/O; it can request a keyframe timeout.
void sc_recording_poll(struct sc_recording *recording, bool *ready, bool *active,
                       char *message, size_t size);
void sc_recording_destroy(struct sc_recording *recording);

#endif
