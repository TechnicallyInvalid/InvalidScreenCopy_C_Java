#ifndef SC_RECORDING_H
#define SC_RECORDING_H

#include "recorder.h"
#include "trait/frame_sink.h"

#define SC_RECORDING_ANDROID_MIC (1u << 1)
#define SC_RECORDING_LOCAL_MIC (1u << 2)

// Calls run on the recording lifecycle worker, never the AppKit thread.
// No microphone is opened by configuring this backend or selecting a toggle.
struct sc_recording_microphone_ops {
    bool (*set_android)(void *userdata, bool enabled, uint32_t session);
    bool (*query_clock)(void *userdata, uint32_t session, uint32_t sequence);
    // false with an empty error means calibration is still pending.
    bool (*clock_ready)(void *userdata, uint32_t session, char *error, size_t size);
    bool (*start_local)(void *userdata, uint32_t session, char *error, size_t size);
    bool (*poll_local)(void *userdata, char *error, size_t size);
    void (*stop_local)(void *userdata);
};

struct sc_recording;
struct sc_recording_request;
struct sc_recording_audio;
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
    struct sc_frame_sink audio_frame_sink;
    struct sc_packet_sink mixed_audio_sink;
    struct sc_recording_audio *mixer;
    const struct sc_recording_microphone_ops *microphone_ops;
    void *microphone_userdata;
    unsigned selected_microphones;
    unsigned capture_microphones;
    unsigned seen_microphones;
    uint32_t microphone_session;
    bool device_output;
    bool audio_frames_ready;
    bool mixing;
    bool mixed_draining;
    bool android_ready;
    bool microphone_cleanup_pending;
    int64_t stop_pts;
    char microphone_error[512];
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
// Configure before stream/controller threads start; backend outlives destroy.
void sc_recording_set_microphone_backend(struct sc_recording *recording,
    const struct sc_recording_microphone_ops *ops, void *userdata, bool device_output);
bool sc_recording_microphones_available(struct sc_recording *recording);
bool sc_recording_set_microphones(struct sc_recording *recording, bool android_mic, bool local_mic);
void sc_recording_get_microphones(struct sc_recording *recording, bool *android_mic, bool *local_mic);
void sc_recording_set_status(struct sc_recording *recording, const char *message);
// These bounded producer calls never perform file I/O. Late/stale sessions are ignored.
void sc_recording_android_status(struct sc_recording *recording, uint32_t session,
                                 uint8_t status, const char *message);
void sc_recording_android_pcm(struct sc_recording *recording, uint32_t session,
                              int64_t pts_us, const uint8_t *pcm, uint16_t bytes);
void sc_recording_local_pcm(struct sc_recording *recording, uint32_t session,
                            int64_t server_pts_us, const float *pcm, size_t frames);
void sc_recording_microphone_error(struct sc_recording *recording, uint32_t session,
                                   const char *message);
// Shutdown-only wait for capture release/remote ACK, not disk finalization.
// A pending file open with no started capture never delays this phase.
bool sc_recording_wait_microphones_stopped(struct sc_recording *recording, sc_tick deadline);
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
