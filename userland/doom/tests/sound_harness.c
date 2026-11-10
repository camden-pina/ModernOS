/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "coast_protocol.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_config.h"
#include "w_wad.h"
#include <uapi/audio.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t sample[8 + 32 + 8192];
static unsigned length, queued, written, eagain_once;
static int16_t recording[32768];
static unsigned recording_size;

long Doom_OSCall(long number, long operation, long pointer, long frames) {
    assert(number == DOOM_SYS_AUDIO);
    if (operation == OS_AUDIO_INFO) {
        *(struct os_audio_info *)pointer = (struct os_audio_info){.version = 1, .rate = 48000,
            .channels = 2, .sample_bits = 16, .queued_frames = queued, .played_frames = written};
        return 0;
    }
    if (operation == OS_AUDIO_RESET) { queued = 0; return 0; }
    assert(operation == OS_AUDIO_WRITE && frames <= OS_AUDIO_MAX_FRAMES);
    if (eagain_once) { eagain_once = 0; errno = EAGAIN; return -1; }
    assert(recording_size + (unsigned)frames * 2 <= sizeof(recording) / sizeof(*recording));
    memcpy(recording + recording_size, (void *)pointer, (size_t)frames * 4);
    recording_size += (unsigned)frames * 2;
    queued += (unsigned)frames;
    written += (unsigned)frames;
    return frames;
}

int Doom_TraceEnabled(void) { return 0; }
int M_CheckParm(char *name) { (void)name; return 0; }
void M_BindVariable(char *name, void *value) { (void)name; (void)value; }
int W_GetNumForName(char *name) { assert(!strcmp(name, "DSPISTOL")); return 1; }
int W_LumpLength(unsigned lump) { assert(lump == 1); return (int)length; }
void *W_CacheLumpNum(int lump, int tag) { (void)tag; assert(lump == 1); return sample; }
void W_ReleaseLumpNum(int lump) { assert(lump == 1); }

static void fixture(unsigned rate, unsigned count, unsigned value) {
    assert(count <= 8192);
    length = 8 + 32 + count;
    memset(sample, 128, sizeof(sample));
    sample[0] = 3; sample[1] = 0;
    sample[2] = rate; sample[3] = rate >> 8;
    unsigned total = count + 32;
    for (unsigned i = 0; i < 4; i++) sample[4 + i] = total >> (8 * i);
    memset(sample + 24, value, count);
    queued = 0;
    recording_size = 0;
}

int main(void) {
    sfxinfo_t sound = {.name = "PISTOL", .lumpnum = 1};
    I_InitSound(1);
    assert(I_GetSfxLumpNum(&sound) == 1);
    fixture(48000, 64, 255);
    assert(I_StartSound(&sound, 0, 127, 0) == 0);
    I_UpdateSound();
    assert(recording_size == 2048 && recording[0] > 7000 && recording[1] == 0);
    assert(recording[126] > 7000 && recording[128] == 0 && !I_SoundIsPlaying(0));

    fixture(48000, 64, 0);
    assert(I_StartSound(&sound, 1, 127, 254) == 1);
    I_UpdateSound();
    assert(recording[0] == 0 && recording[1] == -8192);

    fixture(12000, 64, 255);
    assert(I_StartSound(&sound, 0, 127, 0) == 0);
    I_UpdateSound();
    assert(recording[510] > 7000 && recording[512] == 0);

    fixture(48000, 8192, 255);
    for (int i = 0; i < 8; i++) assert(I_StartSound(&sound, i, 127, 0) == i);
    I_UpdateSound();
    assert(recording[0] == 32767 && recording[1] == 0);
    for (int i = 0; i < 16; i++) I_StopSound(i);

    fixture(48000, 64, 255);
    assert(I_StartSound(&sound, 0, 127, 0) == 0);
    eagain_once = 1;
    I_UpdateSound();
    assert(recording_size == 0 && I_SoundIsPlaying(0));
    I_UpdateSound();
    assert(recording[0] > 7000 && recording[126] > 7000 && !I_SoundIsPlaying(0));

    fixture(48000, 64, 255);
    memset(sample + 4, 255, 4);
    assert(I_StartSound(&sound, 0, 127, 0) == -1);
    fixture(48000, 64, 255);
    sample[0] = 0;
    assert(I_StartSound(&sound, 0, 127, 0) == -1);
    assert(I_StartSound(&sound, -1, 127, 0) == -1);
    assert(I_StartSound(&sound, 16, 127, 0) == -1);
    I_ShutdownSound();
    /* Music and effects share the same transactional native PCM queue. */
    static unsigned char score[]={
        'M','T','h','d',0,0,0,6,0,0,0,1,0,96,
        'M','T','r','k',0,0,0,15,0,0xc0,0,0,0x90,60,100,96,0x80,60,0,0,0xff,0x2f,0};
    I_InitSound(1);I_InitMusic();I_SetMusicVolume(127);
    void *song=I_RegisterSong(score,sizeof(score));assert(song);
    queued=recording_size=0;I_PlaySong(song,0);eagain_once=1;I_UpdateSound();assert(!recording_size);
    I_UpdateSound();assert(recording_size>0);unsigned recorded=recording_size;
    int16_t expected[8192];assert(recorded<=8192);memcpy(expected,recording,recorded*sizeof(*expected));
    I_StopSong();queued=recording_size=0;I_PlaySong(song,0);I_UpdateSound();
    assert(recording_size==recorded&&!memcmp(expected,recording,recorded*sizeof(*expected)));
    queued=recording_size=0;I_PauseSong();I_UpdateSound();assert(!recording_size);I_ResumeSong();
    I_SetMusicVolume(0);I_UpdateSound();assert(recording_size>0);
    for (unsigned i=0;i<recording_size;i++) assert(!recording[i]);
    I_UnRegisterSong(song);I_ShutdownMusic();I_ShutdownSound();
    puts("Doom sound/music mixer host harness passed (including music EAGAIN rollback).");
    return 0;
}
