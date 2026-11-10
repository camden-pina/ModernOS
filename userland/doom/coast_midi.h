/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef COAST_MIDI_H
#define COAST_MIDI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MIDI_RATE 48000
#define MIDI_VOICES 32
struct midi_event {
    uint32_t tick, order, frame, value;
    uint8_t type, channel, a, b;
};
struct midi_song {
    struct midi_event *events;
    uint32_t count, length_frames, hash;
};
struct midi_channel {
    uint8_t program, volume, expression, pan;
    uint16_t bend;
    bool sustain;
};
struct midi_voice {
    uint32_t phase, step, age, noise;
    int level, release_step;
    uint8_t channel, note, velocity, program;
    bool active, released, held;
};
struct midi_player {
    const struct midi_song *song;
    struct midi_channel channels[16];
    struct midi_voice voices[MIDI_VOICES];
    uint32_t cursor, frame;
    uint64_t note_events, rendered_frames, nonzero_frames;
    bool playing, looping, paused, ending;
};
struct midi_song *midi_song_load(const void *data, size_t size);
void midi_song_free(struct midi_song *song);
void midi_player_start(struct midi_player *, const struct midi_song *, bool loop);
void midi_player_stop(struct midi_player *);
void midi_player_render(struct midi_player *, int16_t *stereo, unsigned frames, int volume);
#endif
