/* SPDX-License-Identifier: GPL-2.0-or-later
 * Coast procedural music synthesis. No sampled instrument bank is needed.
 * Bounded MUS/SMF0/1 decoding; GM program families select original synthesized
 * timbres, not an exact General MIDI or original OPL instrument emulation.
 */
#include "coast_midi.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define MAX_EVENTS 131072u
#define MAX_FRAMES (MIDI_RATE * 60u * 60u * 4u)
#define EVENT_TEMPO 1
#define EVENT_END 2
struct reader {
    const uint8_t *at, *end;
};
struct builder {
    struct midi_event *events;
    uint32_t count, capacity;
};
static int16_t sine_wave[1024];
static uint32_t note_step[128];
static bool tables_ready;
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] * 256 + p[1]; }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static unsigned le16(const uint8_t *p) { return p[0] + (unsigned)p[1] * 256; }
static bool byte(struct reader *r, uint8_t *out) {
    if (r->at == r->end)
        return false;
    *out = *r->at++;
    return true;
}
static bool variable(struct reader *r, uint32_t *out) {
    uint32_t value = 0;
    uint8_t c;
    for (unsigned i = 0; i < 4; i++) {
        if (!byte(r, &c))
            return false;
        value = (value << 7) | (c & 127);
        if (!(c & 128)) {
            *out = value;
            return true;
        }
    }
    return false;
}
static bool append(struct builder *b, uint32_t tick, unsigned type, unsigned channel, unsigned a,
                   unsigned c, uint32_t value) {
    if (b->count == MAX_EVENTS)
        return false;
    if (b->count == b->capacity) {
        uint32_t n = b->capacity ? b->capacity * 2 : 1024;
        struct midi_event *p = realloc(b->events, n * sizeof(*p));
        if (!p)
            return false;
        b->events = p;
        b->capacity = n;
    }
    b->events[b->count] = (struct midi_event){tick, b->count, 0, value, type, channel, a, c};
    b->count++;
    return true;
}
static bool smf(struct builder *b, const uint8_t *data, size_t size, unsigned *division) {
    if (size < 14 || memcmp(data, "MThd", 4) || be32(data + 4) < 6 || be32(data + 4) > size - 8)
        return false;
    unsigned format = be16(data + 8), tracks = be16(data + 10);
    *division = be16(data + 12);
    if (format > 1 || !tracks || tracks > 64 || (format == 0 && tracks != 1) || !*division ||
        (*division & 0x8000))
        return false;
    struct reader file = {data + 8 + be32(data + 4), data + size};
    for (unsigned track = 0; track < tracks; track++) {
        if ((size_t)(file.end - file.at) < 8 || memcmp(file.at, "MTrk", 4))
            return false;
        uint32_t length = be32(file.at + 4);
        file.at += 8;
        if (length > (size_t)(file.end - file.at))
            return false;
        struct reader r = {file.at, file.at + length};
        file.at += length;
        uint32_t tick = 0;
        uint8_t running = 0;
        bool ended = false;
        while (r.at < r.end) {
            uint32_t delta;
            uint8_t status, a, c = 0;
            if (!variable(&r, &delta) || delta > UINT32_MAX - tick || !byte(&r, &status))
                return false;
            tick += delta;
            if (status < 128) {
                if (!running)
                    return false;
                r.at--;
                status = running;
            }
            if (status < 0xf0) {
                running = status;
                unsigned kind = status & 0xf0;
                if (kind < 0x80 || kind > 0xe0 || !byte(&r, &a) || a > 127)
                    return false;
                if (kind != 0xc0 && kind != 0xd0 && (!byte(&r, &c) || c > 127))
                    return false;
                if (!append(b, tick, kind, status & 15, a, c, 0))
                    return false;
            } else if (status == 0xff) {
                uint32_t n;
                uint8_t type;
                if (!byte(&r, &type) || !variable(&r, &n) || n > (size_t)(r.end - r.at))
                    return false;
                if (type == 0x51) {
                    if (n != 3)
                        return false;
                    uint32_t tempo = (uint32_t)r.at[0] << 16 | (uint32_t)r.at[1] << 8 | r.at[2];
                    if (!tempo || !append(b, tick, EVENT_TEMPO, 0, 0, 0, tempo))
                        return false;
                }
                if (type == 0x2f) {
                    if (n || !append(b, tick, EVENT_END, 0, 0, 0, 0))
                        return false;
                    ended = true;
                    break;
                }
                r.at += n;
            } else if (status == 0xf0 || status == 0xf7) {
                uint32_t n;
                if (!variable(&r, &n) || n > (size_t)(r.end - r.at))
                    return false;
                r.at += n;
            } else
                return false;
        }
        if (!ended && !append(b, tick, EVENT_END, 0, 0, 0, 0))
            return false;
    }
    return true;
}
static bool mus(struct builder *b, const uint8_t *data, size_t size, unsigned *division) {
    if (size < 16 || memcmp(data, "MUS\032", 4))
        return false;
    unsigned length = le16(data + 4), start = le16(data + 6), instruments = le16(data + 12);
    if (start < 16u + instruments * 2u || start > size || !length || length > size - start ||
        le16(data + 8) > 15)
        return false;
    struct reader r = {data + start, data + start + length};
    static const uint8_t controllers[] = {0,  32, 1,   7,   10,  11,  91, 93,
                                          64, 67, 120, 123, 126, 127, 121};
    uint8_t velocities[16];
    memset(velocities, 127, sizeof(velocities));
    uint32_t tick = 0;
    *division = 70;
    while (r.at < r.end) {
        uint8_t event, a, c = 0;
        if (!byte(&r, &event))
            return false;
        unsigned kind = (event >> 4) & 7, source = event & 15,
                 ch = source == 15  ? 9
                      : source >= 9 ? source + 1
                                    : source;
        if (kind == 6)
            return append(b, tick, EVENT_END, 0, 0, 0, 0);
        if (!byte(&r, &a))
            return false;
        unsigned type;
        if (kind == 0) {
            if (a > 127)
                return false;
            type = 0x80;
        } else if (kind == 1) {
            if (a & 128) {
                if (!byte(&r, &velocities[source]) || velocities[source] > 127)
                    return false;
            }
            a &= 127;
            c = velocities[source];
            type = 0x90;
        } else if (kind == 2) {
            c = a >> 1;
            a = (a & 1) << 6;
            type = 0xe0;
        } else if (kind == 3) {
            if (a < 10 || a > 14)
                return false;
            a = controllers[a];
            type = 0xb0;
        } else if (kind == 4) {
            if (a > 9 || !byte(&r, &c) || c > 127)
                return false;
            if (!a) {
                a = c;
                c = 0;
                type = 0xc0;
            } else {
                a = controllers[a];
                type = 0xb0;
            }
        } else
            return false;
        if (!append(b, tick, type, ch, a, c, 0))
            return false;
        if (event & 128) {
            uint32_t delta;
            if (!variable(&r, &delta) || delta > UINT32_MAX - tick)
                return false;
            tick += delta;
        }
    }
    return false;
}
static int compare(const void *a, const void *b) {
    const struct midi_event *x = a, *y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    return x->order < y->order ? -1 : x->order != y->order;
}
struct midi_song *midi_song_load(const void *memory, size_t size) {
    if (!memory || size > 8 * 1024 * 1024)
        return NULL;
    const uint8_t *data = memory;
    struct builder b = {0};
    unsigned division = 0;
    bool ok = size >= 4 && (!memcmp(data, "MThd", 4) ? smf(&b, data, size, &division)
                                                     : mus(&b, data, size, &division));
    if (!ok || !b.count) {
        free(b.events);
        return NULL;
    }
    qsort(b.events, b.count, sizeof(*b.events), compare);
    uint32_t tick = 0, tempo = 500000;
    uint64_t frame = 0, remainder = 0, denominator = (uint64_t)division * 1000000;
    for (uint32_t i = 0; i < b.count; i++) {
        struct midi_event *e = b.events + i;
        __uint128_t scaled = (__uint128_t)(e->tick - tick) * tempo * MIDI_RATE + remainder;
        if (scaled / denominator > MAX_FRAMES - frame) {
            free(b.events);
            return NULL;
        }
        frame += (uint64_t)(scaled / denominator);
        remainder = (uint64_t)(scaled % denominator);
        e->frame = frame;
        tick = e->tick;
        if (e->type == EVENT_TEMPO)
            tempo = e->value;
    }
    struct midi_song *song = malloc(sizeof(*song));
    if (!song) {
        free(b.events);
        return NULL;
    }
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ data[i]) * 16777619u;
    *song = (struct midi_song){b.events, b.count, (uint32_t)(frame ? frame : 1), hash};
    return song;
}
void midi_song_free(struct midi_song *song) {
    if (song) {
        free(song->events);
        free(song);
    }
}
static void tables(void) {
    if (tables_ready)
        return;
    for (unsigned i = 0; i < 1024; i++)
        sine_wave[i] = (int16_t)(sin(i * 6.283185307179586 / 1024) * 32767);
    for (unsigned i = 0; i < 128; i++)
        note_step[i] =
            (uint32_t)(440.0 * pow(2.0, ((int)i - 69) / 12.0) * 4294967296.0 / MIDI_RATE);
    tables_ready = true;
}
static void reset(struct midi_player *p) {
    memset(p->channels, 0, sizeof(p->channels));
    memset(p->voices, 0, sizeof(p->voices));
    for (unsigned i = 0; i < 16; i++)
        p->channels[i] = (struct midi_channel){0, 100, 127, 64, 8192, false};
    p->cursor = p->frame = 0;
    p->ending = false;
}
void midi_player_start(struct midi_player *p, const struct midi_song *song, bool loop) {
    tables();
    memset(p, 0, sizeof(*p));
    p->song = song;
    p->playing = song != NULL;
    p->looping = loop;
    reset(p);
}
void midi_player_stop(struct midi_player *p) { memset(p, 0, sizeof(*p)); }
static void release(struct midi_voice *v) {
    v->released = true;
    v->held = false;
    v->release_step = v->level / 12000 + 1;
}
static void pitch(struct midi_player *p, unsigned ch) {
    double ratio = pow(2.0, ((int)p->channels[ch].bend - 8192) / 49152.0);
    for (unsigned i = 0; i < MIDI_VOICES; i++)
        if (p->voices[i].active && p->voices[i].channel == ch)
            p->voices[i].step = (uint32_t)(note_step[p->voices[i].note] * ratio);
}
static void dispatch(struct midi_player *p, const struct midi_event *e) {
    struct midi_channel *c = p->channels + e->channel;
    if (e->type == 0x80 || (e->type == 0x90 && !e->b)) {
        for (unsigned i = 0; i < MIDI_VOICES; i++) {
            struct midi_voice *v = p->voices + i;
            if (v->active && v->channel == e->channel && v->note == e->a) {
                if (c->sustain)
                    v->held = true;
                else
                    release(v);
            }
        }
    } else if (e->type == 0x90) {
        unsigned slot = 0;
        uint32_t oldest = 0;
        for (unsigned i = 0; i < MIDI_VOICES; i++) {
            if (!p->voices[i].active) {
                slot = i;
                break;
            }
            if (p->voices[i].age >= oldest) {
                oldest = p->voices[i].age;
                slot = i;
            }
        }
        p->voices[slot] = (struct midi_voice){.step = note_step[e->a],
                                              .noise = 0x9e3779b9u + e->a * 31u + slot,
                                              .channel = e->channel,
                                              .note = e->a,
                                              .velocity = e->b,
                                              .program = c->program,
                                              .active = true};
        pitch(p, e->channel);
        p->note_events++;
    } else if (e->type == 0xc0)
        c->program = e->a;
    else if (e->type == 0xe0) {
        c->bend = e->a + (unsigned)e->b * 128;
        pitch(p, e->channel);
    } else if (e->type == 0xb0) {
        if (e->a == 7)
            c->volume = e->b;
        else if (e->a == 10)
            c->pan = e->b;
        else if (e->a == 11)
            c->expression = e->b;
        else if (e->a == 64) {
            c->sustain = e->b >= 64;
            if (!c->sustain)
                for (unsigned i = 0; i < MIDI_VOICES; i++)
                    if (p->voices[i].channel == e->channel && p->voices[i].held)
                        release(p->voices + i);
        } else if (e->a == 120 || e->a == 123) {
            for (unsigned i = 0; i < MIDI_VOICES; i++)
                if (p->voices[i].channel == e->channel) {
                    if (e->a == 120)
                        p->voices[i].active = false;
                    else
                        release(p->voices + i);
                }
        } else if (e->a == 121) {
            c->volume = 100;
            c->expression = 127;
            c->bend = 8192;
            c->sustain = false;
            pitch(p, e->channel);
            for (unsigned i = 0; i < MIDI_VOICES; i++)
                if (p->voices[i].channel == e->channel && p->voices[i].held)
                    release(p->voices + i);
        }
    }
}
static int voice_sample(struct midi_voice *v) {
    unsigned family = v->program / 8;
    int wave = sine_wave[v->phase >> 22];
    uint32_t x = v->phase >> 16;
    int triangle = x < 32768 ? (int)x * 2 - 32768 : 98303 - (int)x * 2;
    if (v->channel == 9) {
        v->noise ^= v->noise << 13;
        v->noise ^= v->noise >> 17;
        v->noise ^= v->noise << 5;
        if (v->note == 35 || v->note == 36)
            wave = sine_wave[v->phase >> 22];
        else
            wave = (int16_t)v->noise;
        if (!v->age)
            v->level = 32767;
        v->level -= v->note == 42 || v->note == 44 ? 32 : 6;
    } else if (v->released)
        v->level -= v->release_step;
    else {
        int attack = family == 6 || family == 11 ? 14 : 342;
        if (v->age < ((family == 6 || family == 11) ? 2400u : 96u)) {
            v->level += attack;
            if (v->level > 32767)
                v->level = 32767;
        } else if (family == 0 || family == 1 || family == 3) {
            if (v->level > 5000)
                v->level -= 2;
        }
        if (family == 2)
            wave = (wave + sine_wave[(v->phase >> 21) & 1023] / 2) * 2 / 3;
        else if (family == 3 || family == 4 || family == 8)
            wave = triangle;
        else if (family == 6 || family == 7)
            wave = ((int16_t)(v->phase >> 16) + wave) / 2;
        else if (family == 10)
            wave = (v->phase & 0x80000000u) ? 18000 : -18000;
        else if (family == 1)
            wave = (wave + sine_wave[(v->phase >> 20) & 1023] / 2) * 2 / 3;
    }
    if (v->level <= 0) {
        v->active = false;
        return 0;
    }
    v->phase += v->step;
    v->age++;
    return wave * v->level / 32767;
}
void midi_player_render(struct midi_player *p, int16_t *stereo, unsigned frames, int volume) {
    memset(stereo, 0, frames * 2 * sizeof(*stereo));
    if (!p->playing || p->paused || !p->song)
        return;
    if (volume < 0)
        volume = 0;
    if (volume > 127)
        volume = 127;
    for (unsigned f = 0; f < frames; f++) {
        if (p->frame >= p->song->length_frames && !p->ending) {
            if (p->looping)
                reset(p);
            else {
                p->ending = true;
                for (unsigned i = 0; i < MIDI_VOICES; i++)
                    if (p->voices[i].active)
                        release(p->voices + i);
            }
        }
        while (p->cursor < p->song->count && p->song->events[p->cursor].frame <= p->frame)
            dispatch(p, p->song->events + p->cursor++);
        int left = 0, right = 0;
        bool active = false;
        for (unsigned i = 0; i < MIDI_VOICES; i++) {
            struct midi_voice *v = p->voices + i;
            if (!v->active)
                continue;
            struct midi_channel *c = p->channels + v->channel;
            active = true;
            int sample = voice_sample(v) * v->velocity / 127 * c->volume / 127 * c->expression /
                         127 * volume / 127 / 6;
            left += sample * (127 - c->pan) / 127;
            right += sample * c->pan / 127;
        }
        if (left > 32767)
            left = 32767;
        if (left < -32768)
            left = -32768;
        if (right > 32767)
            right = 32767;
        if (right < -32768)
            right = -32768;
        stereo[2 * f] = left;
        stereo[2 * f + 1] = right;
        if (left || right)
            p->nonzero_frames++;
        p->frame++;
        p->rendered_frames++;
        if (p->ending && (!active || p->frame - p->song->length_frames >= MIDI_RATE * 2)) {
            p->playing = false;
            break;
        }
    }
}
