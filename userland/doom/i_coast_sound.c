/* SPDX-License-Identifier: GPL-2.0-or-later
 * Doom DMX sound effects mixed into Coast's native 48kHz stereo S16 queue. */
#include "coast_protocol.h"
#include "coast_midi.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_config.h"
#include "w_wad.h"
#include "z_zone.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/uapi/audio.h"

#define MIX_CHANNELS 16
#define MIX_FRAMES 1024
#define MIX_RATE 48000
#define QUEUE_TARGET 3072
struct channel {
    uint8_t *samples;
    unsigned length, step;
    uint64_t position;
    int left, right;
};
static struct channel channels[MIX_CHANNELS];
static boolean ready, prefix, sfx_enabled;
static bool music_enabled;
static int music_volume=80;
static struct midi_player music;
static uint64_t submitted_frames, nonzero_frames, started_sounds;
int snd_samplerate = MIX_RATE;
int snd_cachesize = 16 * 1024 * 1024;
int snd_maxslicetime_ms = 22;
int snd_sfxdevice = SNDDEVICE_SB;
int snd_musicdevice = SNDDEVICE_GENMIDI;
char *snd_musiccmd = "";

static unsigned little16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned little32(const uint8_t *p) {
    return little16(p) | little16(p + 2) << 16;
}
static int clip(int sample) { return sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample; }

void I_StopSound(int handle) {
    if (handle < 0 || handle >= MIX_CHANNELS) return;
    free(channels[handle].samples);
    memset(&channels[handle], 0, sizeof(channels[handle]));
}

void Doom_AudioStats(void) {
    struct os_audio_info info = {0};
    if (ready) Doom_OSCall(DOOM_SYS_AUDIO, OS_AUDIO_INFO, (long)&info, 0);
    printf("DOOM_AUDIO enabled=%u sounds=%" PRIu64 " submitted=%" PRIu64
           " nonzero=%" PRIu64 " queued=%" PRIu64 " played=%" PRIu64 "\n",
           ready, started_sounds, submitted_frames, nonzero_frames, info.queued_frames, info.played_frames);
    if (music_enabled) printf("DOOM_MUSIC playing=%u paused=%u notes=%" PRIu64 " frames=%" PRIu64 " nonzero=%" PRIu64 "\n", music.playing,music.paused,music.note_events,music.rendered_frames,music.nonzero_frames);
}

void Doom_AudioShutdown(void) {
    if (ready) {
        if (Doom_TraceEnabled()) Doom_AudioStats();
        Doom_OSCall(DOOM_SYS_AUDIO, OS_AUDIO_RESET, 0, 0);
        ready = false;
    }
    for (int i = 0; i < MIX_CHANNELS; i++) I_StopSound(i);
    midi_player_stop(&music);
}

void I_InitSound(boolean use_sfx_prefix) {
    struct os_audio_info info;
    prefix = use_sfx_prefix;
    sfx_enabled=!M_CheckParm("-nosfx") && snd_sfxdevice!=SNDDEVICE_NONE;
    if (M_CheckParm("-nosound")) {
        puts("DOOM_AUDIO disabled by configuration");
        return;
    }
    if (Doom_OSCall(DOOM_SYS_AUDIO, OS_AUDIO_INFO, (long)&info, 0) ||
        info.version != 1 || info.rate != MIX_RATE || info.channels != 2 || info.sample_bits != 16) {
        puts("DOOM_AUDIO unavailable; continuing without sound effects");
        return;
    }
    if (Doom_OSCall(DOOM_SYS_AUDIO, OS_AUDIO_RESET, 0, 0)) {
        perror("doom: audio reset");
        return;
    }
    ready = true;
    puts("DOOM_AUDIO ready: 48000Hz stereo S16, 16 software channels, native PCM");
}

void I_ShutdownSound(void) { Doom_AudioShutdown(); }

int I_GetSfxLumpNum(sfxinfo_t *sfx) {
    char name[9];
    sfxinfo_t *base = sfx->link ? sfx->link : sfx;
    snprintf(name, sizeof(name), prefix ? "DS%.6s" : "%.8s", base->name);
    return W_GetNumForName(name);
}

void I_UpdateSoundParams(int handle, int volume, int separation) {
    if (handle < 0 || handle >= MIX_CHANNELS) return;
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    if (separation < 0) separation = 0;
    if (separation > 254) separation = 254;
    channels[handle].left = volume * (254 - separation) / 254;
    channels[handle].right = volume * separation / 254;
}

int I_StartSound(sfxinfo_t *sfx, int handle, int volume, int separation) {
    if (!ready || !sfx_enabled || handle < 0 || handle >= MIX_CHANNELS) return -1;
    I_StopSound(handle);
    int lump = sfx->lumpnum;
    if (lump < 0) lump = I_GetSfxLumpNum(sfx);
    int length = W_LumpLength(lump);
    if (length < 40 || length > 1024 * 1024) return -1;
    const uint8_t *data = W_CacheLumpNum(lump, PU_STATIC);
    unsigned rate = little16(data + 2), samples = little32(data + 4);
    if (little16(data) != 3 || rate < 1000 || rate > 96000 || samples <= 32 ||
        samples > (unsigned)length - 8) {
        W_ReleaseLumpNum(lump);
        return -1;
    }
    /* DMX adds 16 guard samples at each end; they are not audible content. */
    unsigned count = samples - 32;
    uint8_t *copy = malloc(count);
    if (!copy) { W_ReleaseLumpNum(lump); return -1; }
    memcpy(copy, data + 8 + 16, count);
    W_ReleaseLumpNum(lump);
    channels[handle] = (struct channel){.samples = copy, .length = count,
        .step = (unsigned)(((uint64_t)rate << 16) / MIX_RATE)};
    I_UpdateSoundParams(handle, volume, separation);
    started_sounds++;
    if (Doom_TraceEnabled())
        printf("DOOM_SFX start=%" PRIu64 " name=%.8s channel=%d rate=%u samples=%u\n",
               started_sounds, sfx->name, handle, rate, count);
    return handle;
}

boolean I_SoundIsPlaying(int handle) {
    return ready && handle >= 0 && handle < MIX_CHANNELS && channels[handle].samples != NULL;
}

void I_UpdateSound(void) {
    if (!ready) return;
    for (unsigned block = 0; block < 4; block++) {
        struct os_audio_info info;
        if (Doom_OSCall(DOOM_SYS_AUDIO, OS_AUDIO_INFO, (long)&info, 0)) {
            perror("doom: audio info");
            Doom_AudioShutdown();
            return;
        }
        if (info.queued_frames >= QUEUE_TARGET) return;
        bool active = music_enabled && music.playing && !music.paused;
        uint64_t positions[MIX_CHANNELS];
        for (int c = 0; c < MIX_CHANNELS; c++) {
            positions[c] = channels[c].position;
            if (channels[c].samples) active = true;
        }
        if (!active) return;
        int16_t pcm[MIX_FRAMES * 2];
        struct midi_player music_next=music;
        midi_player_render(&music_next,pcm,MIX_FRAMES,music_volume);
        unsigned nonzero = 0;
        for (unsigned frame = 0; frame < MIX_FRAMES; frame++) {
            int left = pcm[2*frame], right = pcm[2*frame+1];
            for (int c = 0; c < MIX_CHANNELS; c++) {
                struct channel *channel = &channels[c];
                unsigned offset = positions[c] >> 16;
                if (!channel->samples || offset >= channel->length) continue;
                int first = channel->samples[offset];
                int next = channel->samples[offset + (offset + 1 < channel->length)];
                int sample = (first - 128) * 256 +
                    (next - first) * (int)(positions[c] & 65535) / 256;
                left += sample * channel->left / (127 * 4);
                right += sample * channel->right / (127 * 4);
                positions[c] += channel->step;
            }
            pcm[2 * frame] = (int16_t)clip(left);
            pcm[2 * frame + 1] = (int16_t)clip(right);
            if (left || right) nonzero++;
        }
        long result = Doom_OSCall(DOOM_SYS_AUDIO, OS_AUDIO_WRITE, (long)pcm, MIX_FRAMES);
        if (result < 0 && errno == EAGAIN) return;
        if (result != MIX_FRAMES) {
            perror("doom: PCM write");
            Doom_AudioShutdown();
            return;
        }
        music=music_next;
        submitted_frames += MIX_FRAMES;
        nonzero_frames += nonzero;
        for (int c = 0; c < MIX_CHANNELS; c++) {
            channels[c].position = positions[c];
            if (channels[c].samples && positions[c] >> 16 >= channels[c].length) I_StopSound(c);
        }
    }
}

void I_PrecacheSounds(sfxinfo_t *sounds, int count) { (void)sounds; (void)count; }
void I_BindSoundVariables(void) {
    M_BindVariable("snd_musicdevice", &snd_musicdevice);
    M_BindVariable("snd_sfxdevice", &snd_sfxdevice);
    M_BindVariable("snd_samplerate", &snd_samplerate);
    M_BindVariable("snd_cachesize", &snd_cachesize);
    M_BindVariable("snd_maxslicetime_ms", &snd_maxslicetime_ms);
    M_BindVariable("snd_musiccmd", &snd_musiccmd);
}

/* Music uses original procedural timbres and the freely distributed WAD score. */
void I_InitMusic(void) {
    music_enabled=!M_CheckParm("-nosound")&&!M_CheckParm("-nomusic")&&snd_musicdevice!=SNDDEVICE_NONE;
    if (music_enabled) puts("DOOM_MUSIC ready: MUS/SMF, procedural GM-inspired synthesis, no sampled bank");
}
void I_ShutdownMusic(void) { midi_player_stop(&music);music_enabled=false; }
void I_SetMusicVolume(int volume) { music_volume=volume<0?0:volume>127?127:volume; }
void I_PauseSong(void) { music.paused=true; }
void I_ResumeSong(void) { music.paused=false; }
void *I_RegisterSong(void *data,int length) {
    if (!music_enabled||length<=0) return NULL;
    struct midi_song *song=midi_song_load(data,(size_t)length);
    if (song&&Doom_TraceEnabled()) printf("DOOM_MUSIC score hash=%08x events=%u frames=%u\n",song->hash,song->count,song->length_frames);
    if (!song) fputs("doom: unsupported or malformed music score\n",stderr);
    return song;
}
void I_UnRegisterSong(void *song) { if (music.song==song) midi_player_stop(&music);midi_song_free(song); }
void I_PlaySong(void *song,boolean looping) { if (music_enabled) midi_player_start(&music,song,looping); }
void I_StopSong(void) { midi_player_stop(&music); }
boolean I_MusicIsPlaying(void) { return music_enabled&&music.playing; }
