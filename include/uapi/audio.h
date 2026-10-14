#ifndef MODERNOS_AUDIO_PROTOCOL_H
#define MODERNOS_AUDIO_PROTOCOL_H
#include <stdint.h>

#define MODERNOS_AUDIO 412

enum { OS_AUDIO_INFO, OS_AUDIO_WRITE, OS_AUDIO_RESET };
#define OS_AUDIO_MAX_FRAMES 2048U

struct os_audio_info {
    uint32_t version, rate, channels, sample_bits;
    uint64_t queued_frames, played_frames;
};

#endif
