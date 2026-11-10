/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "coast_midi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const unsigned char score[] = {'M',  'T', 'h', 'd', 0,    0,  0, 6, 0,    0,    0,    1, 0,
                                      96,   'M', 'T', 'r', 'k',  0,  0, 0, 15,   0,    0xc0, 0, 0,
                                      0x90, 60,  100, 96,  0x80, 60, 0, 0, 0xff, 0x2f, 0};
static unsigned nonzero(const int16_t *pcm, unsigned frames) {
    unsigned n = 0;
    for (unsigned i = 0; i < frames * 2; i++)
        n += pcm[i] != 0;
    return n;
}
int main(int argc, char **argv) {
    for (unsigned n = 0; n < sizeof(score); n++)
        assert(!midi_song_load(score, n));
    unsigned char broken[sizeof(score)];
    memcpy(broken, score, sizeof(score));
    broken[12] = 0x80;
    assert(!midi_song_load(broken, sizeof(broken)));
    memcpy(broken, score, sizeof(score));
    broken[27] = 0x90;
    assert(!midi_song_load(broken, sizeof(broken)));
    struct midi_song *song = midi_song_load(score, sizeof(score));
    assert(song && song->length_frames == 24000);
    struct midi_player player;
    int16_t pcm[2048];
    midi_player_start(&player, song, false);
    midi_player_render(&player, pcm, 1024, 127);
    assert(nonzero(pcm, 1024) > 100 && player.note_events == 1 && player.frame == 1024);
    player.paused = true;
    uint32_t frame = player.frame;
    midi_player_render(&player, pcm, 1024, 127);
    assert(!nonzero(pcm, 1024) && player.frame == frame);
    player.paused = false;
    midi_player_render(&player, pcm, 1024, 0);
    assert(!nonzero(pcm, 1024) && player.frame == frame + 1024);
    for (unsigned i = 0; i < 150; i++)
        midi_player_render(&player, pcm, 1024, 127);
    assert(!player.playing);
    midi_player_start(&player, song, true);
    for (unsigned i = 0; i < 70; i++)
        midi_player_render(&player, pcm, 1024, 127);
    assert(player.playing && player.note_events >= 3);
    midi_song_free(song);
    const unsigned char mus[] = {'M', 'U', 'S', 26, 6, 0,    16,   0,  1,    0,    0,
                                 0,   0,   0,   0,  0, 0x90, 0xbc, 90, 0x81, 0x0c, 0x60};
    song = midi_song_load(mus, sizeof(mus));
    assert(song && song->length_frames == 48000);
    midi_song_free(song);
    for (unsigned n = 0; n < sizeof(mus); n++)
        assert(!midi_song_load(mus, n));
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        assert(f);
        assert(!fseek(f, 0, SEEK_END));
        long size = ftell(f);
        assert(size > 0 && size <= 8 * 1024 * 1024);
        rewind(f);
        unsigned char *data = malloc((size_t)size);
        assert(data && fread(data, 1, (size_t)size, f) == (size_t)size);
        fclose(f);
        song = midi_song_load(data, (size_t)size);
        assert(song);
        midi_player_start(&player, song, true);
        for (unsigned i = 0; i < 300; i++)
            midi_player_render(&player, pcm, 1024, 100);
        assert(player.note_events > 0 && player.nonzero_frames > 1000);
        printf("MIDI SCORE PASS events=%u frames=%u notes=%llu nonzero=%llu\n", song->count,
               song->length_frames, (unsigned long long)player.note_events,
               (unsigned long long)player.nonzero_frames);
        midi_song_free(song);
        free(data);
    }
    puts("PASS MIDI bounds, timing, MUS, synthesis, pause, mute, loop and stop");
    return 0;
}
