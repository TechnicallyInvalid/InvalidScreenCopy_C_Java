#include "common.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_msg.h"
#include "util/binary.h"

static void test_deserialize_clipboard(void) {
    const uint8_t input[] = {
        DEVICE_MSG_TYPE_CLIPBOARD,
        0x00, 0x00, 0x00, 0x03, // text length
        0x41, 0x42, 0x43, // "ABC"
    };

    struct sc_device_msg msg;
    ssize_t r = sc_device_msg_deserialize(input, sizeof(input), &msg);
    assert(r == 8);

    assert(msg.type == DEVICE_MSG_TYPE_CLIPBOARD);
    assert(msg.clipboard.text);
    assert(!strcmp("ABC", msg.clipboard.text));

    sc_device_msg_destroy(&msg);
}

static void test_deserialize_clipboard_big(void) {
    uint8_t input[DEVICE_MSG_MAX_SIZE];
    input[0] = DEVICE_MSG_TYPE_CLIPBOARD;
    input[1] = (DEVICE_MSG_TEXT_MAX_LENGTH & 0xff000000u) >> 24;
    input[2] = (DEVICE_MSG_TEXT_MAX_LENGTH & 0x00ff0000u) >> 16;
    input[3] = (DEVICE_MSG_TEXT_MAX_LENGTH & 0x0000ff00u) >> 8;
    input[4] =  DEVICE_MSG_TEXT_MAX_LENGTH & 0x000000ffu;

    memset(input + 5, 'a', DEVICE_MSG_TEXT_MAX_LENGTH);

    struct sc_device_msg msg;
    ssize_t r = sc_device_msg_deserialize(input, sizeof(input), &msg);
    assert(r == DEVICE_MSG_MAX_SIZE);

    assert(msg.type == DEVICE_MSG_TYPE_CLIPBOARD);
    assert(msg.clipboard.text);
    assert(strlen(msg.clipboard.text) == DEVICE_MSG_TEXT_MAX_LENGTH);
    assert(msg.clipboard.text[0] == 'a');

    sc_device_msg_destroy(&msg);
}

static void test_deserialize_ack_set_clipboard(void) {
    const uint8_t input[] = {
        DEVICE_MSG_TYPE_ACK_CLIPBOARD,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // sequence
    };

    struct sc_device_msg msg;
    ssize_t r = sc_device_msg_deserialize(input, sizeof(input), &msg);
    assert(r == 9);

    assert(msg.type == DEVICE_MSG_TYPE_ACK_CLIPBOARD);
    assert(msg.ack_clipboard.sequence == UINT64_C(0x0102030405060708));
}

static void test_deserialize_uhid_output(void) {
    const uint8_t input[] = {
        DEVICE_MSG_TYPE_UHID_OUTPUT,
        0, 42, // id
        0, 5, // size
        0x01, 0x02, 0x03, 0x04, 0x05, // data
    };

    struct sc_device_msg msg;
    ssize_t r = sc_device_msg_deserialize(input, sizeof(input), &msg);
    assert(r == 10);

    assert(msg.type == DEVICE_MSG_TYPE_UHID_OUTPUT);
    assert(msg.uhid_output.id == 42);
    assert(msg.uhid_output.size == 5);

    uint8_t expected[] = {1, 2, 3, 4, 5};
    assert(!memcmp(msg.uhid_output.data, expected, sizeof(expected)));

    sc_device_msg_destroy(&msg);
}

static void assert_fragmented(const uint8_t *input, size_t size) {
    struct sc_device_msg msg;
    for (size_t len = 0; len < size; ++len) {
        assert(!sc_device_msg_deserialize(input, len, &msg));
    }
}

static void test_microphone_status(void) {
    const uint8_t input[] = {
        3, 0x89, 0xab, 0xcd, 0xef, 2, 0, 7,
        'D', 'e', 'n', 'i', 'e', 'd', '!',
    };
    assert_fragmented(input, sizeof(input));
    struct sc_device_msg msg;
    assert(sc_device_msg_deserialize(input, sizeof(input), &msg) == sizeof(input));
    assert(msg.type == DEVICE_MSG_TYPE_MIC_STATUS);
    assert(msg.mic_status.session == UINT32_C(0x89abcdef));
    assert(msg.mic_status.status == SC_MIC_STATUS_ERROR);
    assert(!strcmp(msg.mic_status.text, "Denied!"));
    sc_device_msg_destroy(&msg); // inline text requires no free

    uint8_t maximum[8 + SC_MIC_STATUS_TEXT_MAX_LENGTH];
    memcpy(maximum, input, 8);
    sc_write16be(&maximum[6], SC_MIC_STATUS_TEXT_MAX_LENGTH);
    memset(maximum + 8, 'a', SC_MIC_STATUS_TEXT_MAX_LENGTH);
    assert_fragmented(maximum, sizeof(maximum));
    assert(sc_device_msg_deserialize(maximum, sizeof(maximum), &msg) == sizeof(maximum));
    assert(strlen(msg.mic_status.text) == SC_MIC_STATUS_TEXT_MAX_LENGTH);
    maximum[5] = 3; // invalid status fails immediately with complete header
    assert(sc_device_msg_deserialize(maximum, 8, &msg) == -1);
    maximum[5] = 0;
    sc_write16be(&maximum[6], SC_MIC_STATUS_TEXT_MAX_LENGTH + 1);
    assert(sc_device_msg_deserialize(maximum, 8, &msg) == -1);
    sc_write16be(&maximum[6], 0);
    assert(sc_device_msg_deserialize(maximum, 8, &msg) == 8);
    assert(!msg.mic_status.text[0]);

    // UTF-8 status text accepts complete multibyte code points, never a
    // truncated sequence or an embedded NUL that hides diagnostic details.
    const uint8_t valid[] = {0xc3, 0xa9, 0xe2, 0x9c, 0x93, 0xf0, 0x9f, 0x8e, 0xa4};
    sc_write16be(&maximum[6], sizeof(valid));
    memcpy(maximum + 8, valid, sizeof(valid));
    assert(sc_device_msg_deserialize(maximum, 8 + sizeof(valid), &msg) == 8 + sizeof(valid));
    sc_write16be(&maximum[6], 1);
    for (unsigned value = 0; value < 256; ++value) {
        maximum[8] = value;
        ssize_t result = sc_device_msg_deserialize(maximum, 9, &msg);
        assert(result == (value > 0 && value < 128 ? 9 : -1));
    }
    const uint8_t invalid[][4] = {
        {0xc0, 0x80, 'a', 'a'}, // overlong
        {0xed, 0xa0, 0x80, 'a'}, // surrogate
        {0xf4, 0x90, 0x80, 0x80}, // above U+10FFFF
        {0xe2, 'a', 0x80, 'a'}, // non-continuation
    };
    sc_write16be(&maximum[6], 4);
    for (size_t i = 0; i < ARRAY_LEN(invalid); ++i) {
        memcpy(maximum + 8, invalid[i], 4);
        assert(sc_device_msg_deserialize(maximum, 12, &msg) == -1);
    }
}

static void test_microphone_pcm_and_clock(void) {
    uint8_t input[15 + SC_MIC_PCM_MAX_SIZE + 17] = {4, 0x89, 0xab, 0xcd, 0xef};
    const int64_t pts = INT64_C(0x0102030405060708);
    sc_write64be(&input[5], pts);
    sc_write16be(&input[13], SC_MIC_PCM_MAX_SIZE);
    for (size_t i = 0; i < SC_MIC_PCM_MAX_SIZE; ++i) input[15 + i] = i;
    size_t pcm_size = 15 + SC_MIC_PCM_MAX_SIZE;
    assert_fragmented(input, pcm_size);
    struct sc_device_msg msg;
    assert(sc_device_msg_deserialize(input, pcm_size, &msg) == (ssize_t) pcm_size);
    assert(msg.type == DEVICE_MSG_TYPE_MIC_PCM);
    assert(msg.mic_pcm.session == UINT32_C(0x89abcdef));
    assert(msg.mic_pcm.pts == pts);
    assert(msg.mic_pcm.size == SC_MIC_PCM_MAX_SIZE);
    assert(!memcmp(msg.mic_pcm.data, input + 15, SC_MIC_PCM_MAX_SIZE));
    sc_device_msg_destroy(&msg);

    uint8_t *clock = input + pcm_size;
    clock[0] = 5;
    sc_write32be(clock + 1, UINT32_C(0x89abcdef));
    sc_write32be(clock + 5, UINT32_C(0xfefdfcfb));
    sc_write64be(clock + 9, pts + 20000);
    assert_fragmented(clock, 17);
    // Coalesced TCP messages consume exactly one frame at a time.
    assert(sc_device_msg_deserialize(input, sizeof(input), &msg) == (ssize_t) pcm_size);
    assert(sc_device_msg_deserialize(clock, 17, &msg) == 17);
    assert(msg.type == DEVICE_MSG_TYPE_CLOCK);
    assert(msg.clock.session == UINT32_C(0x89abcdef));
    assert(msg.clock.sequence == UINT32_C(0xfefdfcfb));
    assert(msg.clock.device_us == pts + 20000);
    sc_device_msg_destroy(&msg);
    clock[9] = 0x80;
    assert(sc_device_msg_deserialize(clock, 17, &msg) == -1);

    const uint16_t invalid_sizes[] = {0, 1, 1919, 1921, 1922, UINT16_MAX};
    for (size_t i = 0; i < ARRAY_LEN(invalid_sizes); ++i) {
        sc_write16be(input + 13, invalid_sizes[i]);
        assert(sc_device_msg_deserialize(input, 15, &msg) == -1);
    }
    sc_write16be(input + 13, 2);
    assert(sc_device_msg_deserialize(input, 17, &msg) == 17);
    input[5] = 0x80;
    assert(sc_device_msg_deserialize(input, 15, &msg) == -1);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    test_deserialize_clipboard();
    test_deserialize_clipboard_big();
    test_deserialize_ack_set_clipboard();
    test_deserialize_uhid_output();
    test_microphone_status();
    test_microphone_pcm_and_clock();
    return 0;
}
