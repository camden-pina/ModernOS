#define _GNU_SOURCE
#include "include/audio_protocol.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void rest(void) {
    struct timespec t = {0, 5000000};
    nanosleep(&t, NULL);
}

static int info(struct os_audio_info *p) {
    if (syscall(MODERNOS_AUDIO, OS_AUDIO_INFO, p, 0) < 0) {
        perror("speaker-check: audio info");
        return -1;
    }
    if (p->version != 1 || p->rate != 48000 || p->channels != 2 || p->sample_bits != 16) {
        fputs("speaker-check: unsupported PCM format\n", stderr);
        return -1;
    }
    return 0;
}

static int drain(void) {
    double deadline = now() + 5;
    struct os_audio_info p;
    while (now() < deadline) {
        if (info(&p))
            return -1;
        if (!p.queued_frames)
            return 0;
        rest();
    }

    fputs("speaker-check: PCM device did not drain\n", stderr);
    return -1;
}

static int tone(unsigned channels, double frequency, double seconds) {
    int16_t samples[1024 * 2];
    unsigned frames = (unsigned)(48000 * seconds), sent = 0;
    double deadline = now() + seconds + 5;
    while (sent < frames && now() < deadline) {
        struct os_audio_info p;
        if (info(&p))
            return -1;
        if (p.queued_frames > 9600) {
            rest();
            continue;
        }
        unsigned n = frames - sent < 1024 ? frames - sent : 1024;
        for (unsigned i = 0; i < n; i++) {
            unsigned frame = sent + i;
            /* Quiet 6% PCM amplitude and 20-ms edges avoid abrupt transients. */
            double fade = frame < 960            ? frame / 960.0
                          : frames - frame < 960 ? (frames - frame) / 960.0
                                                 : 1.0;
            int16_t value =
                (int16_t)(2000 * fade * sin(6.283185307179586 * frequency * frame / 48000));
            samples[2 * i] = channels & 1 ? value : 0;
            samples[2 * i + 1] = channels & 2 ? value : 0;
        }

        long result = syscall(MODERNOS_AUDIO, OS_AUDIO_WRITE, samples, n);
        if (result < 0 && errno == EAGAIN) {
            rest();
            continue;
        }
        if (result != (long)n) {
            perror("speaker-check: PCM/amp start");
            return -1;
        }
        sent += n;
    }

    if (sent != frames) {
        fputs("speaker-check: PCM queue stalled\n", stderr);
        return -1;
    }
    return drain();
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--info")) {
        struct os_audio_info p;
        if (info(&p))
            return 1;
        printf("PCM: %u Hz, %u channels, %u bits, queued=%llu played=%llu frames\n", p.rate,
               p.channels, p.sample_bits, (unsigned long long)p.queued_frames,
               (unsigned long long)p.played_frames);
        return 0;
    }

    if (argc != 1) {
        puts("Usage: speaker-check [--info]");
        return 2;
    }
    puts(
        "Speaker listening check: quiet LEFT 440 Hz, RIGHT 660 Hz, BOTH 440 Hz, two-second silence, resume.");
    if (syscall(MODERNOS_AUDIO, OS_AUDIO_RESET, 0, 0) < 0) {
        perror("speaker-check: reset");
        return 1;
    }
    puts("LEFT only");
    fflush(stdout);
    if (tone(1, 440, 2))
        goto failed;
    puts("RIGHT only");
    fflush(stdout);
    if (tone(2, 660, 2))
        goto failed;
    puts("BOTH");
    fflush(stdout);
    if (tone(3, 440, 2))
        goto failed;
    if (syscall(MODERNOS_AUDIO, OS_AUDIO_RESET, 0, 0) < 0)
        goto failed;
    puts("PAUSE: two seconds of silence");
    fflush(stdout);
    struct os_audio_info paused, resumed;
    if (info(&paused) || paused.queued_frames)
        goto failed;
    double pause_start = now();
    struct timespec pause = {2, 0};
    while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {
    }
    double elapsed = now() - pause_start;
    if (info(&resumed) || resumed.queued_frames || resumed.played_frames != paused.played_frames ||
        elapsed < 1.9) {
        fputs("speaker-check: paused device clock changed\n", stderr);
        goto failed;
    }
    printf("PAUSE verified: %.3f seconds, no queued PCM, device clock held\n", elapsed);
    fflush(stdout);
    puts("RESUME: both speakers");
    fflush(stdout);
    if (tone(3, 440, 2))
        goto failed;
    if (syscall(MODERNOS_AUDIO, OS_AUDIO_RESET, 0, 0) < 0)
        goto failed;
    puts(
        "PCM/stereo/pause/resume sequence submitted and drained. A person must confirm audible speakers and silence.");
    return 0;
failed:
    syscall(MODERNOS_AUDIO, OS_AUDIO_RESET, 0, 0);
    return 1;
}
