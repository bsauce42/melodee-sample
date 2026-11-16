// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// The stem separator Melodee runs beside itself (src/melodee/shell/stems.h,
// tools/stems/build.py): Spleeter's 4-stem separation as Deezer's own program
// does it, on SpleeterRT's network and STFT code.
//
//   melodee-stems <model> <threads> <file.wav>
//
// <model> is Deezer's 4-stem model as tools/stems/convert_model.py writes it.
// <file.wav> is stereo (or mono) at 44.1 kHz. It writes <file.wav>_Drums.wav,
// _Bass.wav, _Music.wav and _Vocals.wav beside it (44.1 kHz, 32-bit float,
// stereo, the input's length to the frame) and prints "progress N" (0 to 100)
// as it goes.
//
// What it does, and SpleeterRT's own offline program does not: that one holds
// a drums and a vocals network, multiplies the sound by each one's mask and
// calls what is left the music. Here each of the four stems has its network
// (vocals, drums, bass, other), each estimates its stem from the whole mix,
// and every bin of the mix is then shared between the four by the squares of
// their estimates (the ratio mask), so the stems add up to the mix and a
// stem's network never has the last word alone. The networks see what they
// were trained on: 512 frames of 1024 bins (up to 11 kHz) a pass. Above
// 11 kHz, where they have no estimate, each frame is shared as its top
// quarter below 11 kHz was.
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include "spleeter.h"
#include "stftFix.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "dr_wav.h"
#pragma GCC diagnostic pop

void openblas_set_num_threads(int threads);

enum { STEMS = 4, FRAMES = 512, BINS = 1024, TOP = 256, RATE = 44100 };
static const char MAGIC[4] = {'M', 'L', 'S', 'T'};
// The files in the order Melodee lands them, and the network of the model each is (vocals, drums, bass, other).
static const char* const stem_file[STEMS] = {"_Drums.wav", "_Bass.wav", "_Music.wav", "_Vocals.wav"};
static const int stem_net[STEMS] = {1, 2, 3, 0};

static void fail(const char* what) {
    fprintf(stderr, "melodee-stems: %s\n", what);
    exit(1);
}

static void progress(int percent) { printf("progress %d\n", percent); }

// An IEEE 754 half float; its denormals read as zero, as SpleeterRT reads its own.
static float half(uint16_t in) {
    uint32_t bits = (uint32_t)(in & 0x7fff) << 13;
    bits += 0x38000000u;
    if (!(in & 0x7c00)) bits = 0;
    bits |= (uint32_t)(in & 0x8000) << 16;
    float out;
    memcpy(&out, &bits, sizeof out);
    return out;
}

// The model's four networks as floats, each a spleeterCoeff.
static float* load_model(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) fail("cannot open the model");
    unsigned char head[16];
    uint32_t words[3];
    if (fread(head, 1, sizeof head, file) != sizeof head || memcmp(head, MAGIC, 4)) fail("not a model file");
    memcpy(words, head + 4, sizeof words);
    const size_t weights = getCoeffSize() / sizeof(float);
    if (words[0] != 1 || words[1] != STEMS || words[2] != weights) fail("the model is not the 4-stem one");
    float* out = (float*)malloc(STEMS * weights * sizeof(float));
    uint16_t* piece = (uint16_t*)malloc(65536 * sizeof(uint16_t));
    if (!out || !piece) fail("out of memory");
    for (size_t at = 0; at < STEMS * weights;) {
        size_t want = STEMS * weights - at;
        if (want > 65536) want = 65536;
        if (fread(piece, sizeof(uint16_t), want, file) != want) fail("the model is cut short");
        for (size_t n = 0; n < want; n++) out[at + n] = half(piece[n]);
        at += want;
    }
    free(piece);
    fclose(file);
    return out;
}

// One job: the mix's spectrum, the networks, and the masks as they are worked out.
typedef struct {
    size_t frames;                 // of spectrum
    float *re[2], *im[2];          // the mix: frames x FFTSIZE each, left and right
    const float* model;
    size_t chunks, next, done;     // a task is one network on one chunk of FRAMES frames: STEMS a chunk
    float* (*estimates)[STEMS];    // chunk, network: the network's own mask, until the chunk's four are in
    uint16_t* share[STEMS];        // network: its share of each bin, frames x 2 sides x BINS, 0..65535
    pthread_mutex_t lock;
} Job;

// A chunk's four masks into each network's share of every bin: the squares of what each estimates, over their sum.
static void share_chunk(Job* job, size_t chunk, float* const estimate[STEMS]) {
    const size_t first = chunk * FRAMES;
    for (size_t t = 0; t < FRAMES && first + t < job->frames; t++) {
        for (int side = 0; side < 2; side++) {
            const float* re = job->re[side] + (first + t) * FFTSIZE;
            const float* im = job->im[side] + (first + t) * FFTSIZE;
            const size_t in = (size_t)side * FRAMES * BINS + t * BINS, out = ((first + t) * 2 + (size_t)side) * BINS;
            for (size_t bin = 0; bin < BINS; bin++) {
                const float level = hypotf(re[bin], im[bin]) * (float)FFTSIZE, power = level * level;
                float each[STEMS], sum = 1e-10f;
                for (int n = 0; n < STEMS; n++) {
                    const float mask = estimate[n][in + bin];
                    each[n] = power * mask * mask + 1e-10f / STEMS;
                    sum += power * mask * mask;
                }
                for (int n = 0; n < STEMS; n++) job->share[n][out + bin] = (uint16_t)lrintf(65535.0f * each[n] / sum);
            }
        }
    }
}

static void* work(void* given) {
    Job* job = (Job*)given;
    float* level = (float*)malloc(2 * FRAMES * BINS * sizeof(float));
    if (!level) fail("out of memory");
    for (;;) {
        pthread_mutex_lock(&job->lock);
        const size_t task = job->next++;
        pthread_mutex_unlock(&job->lock);
        if (task >= job->chunks * STEMS) break;
        const size_t chunk = task / STEMS, first = chunk * FRAMES;
        const int net = (int)(task % STEMS);
        // The chunk's levels, as the networks were trained on them: side, frame, bin. Past the end: silence.
        for (int side = 0; side < 2; side++)
            for (size_t t = 0; t < FRAMES; t++) {
                float* row = level + (size_t)side * FRAMES * BINS + t * BINS;
                if (first + t >= job->frames) { memset(row, 0, BINS * sizeof(float)); continue; }
                const float* re = job->re[side] + (first + t) * FFTSIZE;
                const float* im = job->im[side] + (first + t) * FFTSIZE;
                for (size_t bin = 0; bin < BINS; bin++) row[bin] = hypotf(re[bin], im[bin]) * (float)FFTSIZE;
            }
        spleeter nn = (spleeter)allocateSpleeterStr();
        float* mask = NULL;
        float* kept = (float*)malloc(2 * FRAMES * BINS * sizeof(float));
        if (!nn || !kept) fail("out of memory");
        initSpleeter(nn, BINS, FRAMES, 1, (void*)(job->model + (size_t)net * (getCoeffSize() / sizeof(float))));
        getMaskPtr(nn, &mask);
        processSpleeter(nn, level, mask);
        memcpy(kept, mask, 2 * FRAMES * BINS * sizeof(float));
        freeSpleeter(nn);
        free(nn);
        float* ready[STEMS];
        int whole = 1;
        pthread_mutex_lock(&job->lock);
        job->estimates[chunk][net] = kept;
        for (int n = 0; n < STEMS; n++) {
            ready[n] = job->estimates[chunk][n];
            whole = whole && ready[n];
        }
        progress(8 + (int)(80 * ++job->done / (job->chunks * STEMS)));
        pthread_mutex_unlock(&job->lock);
        if (whole) { // the chunk's last network: no other thread touches its four again
            share_chunk(job, chunk, ready);
            for (int n = 0; n < STEMS; n++) free(ready[n]);
        }
    }
    free(level);
    return NULL;
}

// As a background job on the MPC: it ends with Melodee, holds none of its open files, runs below everything Melodee
// runs, and is the first thing the kernel ends when memory runs out.
static void settle(void) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) exit(4);
    for (int fd = 3; fd < 1024; fd++) close(fd);
    setvbuf(stdout, NULL, _IOLBF, 0); // a line as it is printed: a pipe would hold them back to the end
    struct sched_param normal = {0};
    sched_setscheduler(0, SCHED_OTHER, &normal);
    setpriority(PRIO_PROCESS, 0, 19);
    FILE* oom = fopen("/proc/self/oom_score_adj", "w");
    if (oom) {
        fputs("1000\n", oom);
        fclose(oom);
    }
}

int main(int argc, char* argv[]) {
    settle();
    if (argc != 4) fail("usage: melodee-stems <model> <threads> <file.wav>");
    int threads = atoi(argv[2]);
    if (threads < 1) threads = 1;
    if (threads > 8) threads = 8;
    openblas_set_num_threads(1); // the threads here are the job's own

    unsigned int channels = 0, rate = 0;
    drwav_uint64 length = 0;
    float* sound = drwav_open_file_and_read_pcm_frames_f32(argv[3], &channels, &rate, &length, NULL);
    if (!sound || !length || channels < 1) fail("cannot read the file");
    if (rate != RATE) fail("the file is not at 44.1 kHz");
    progress(1);
    float* model = load_model(argv[1]);
    progress(5);

    // The sound a window in from the start, and a window and more of silence after it, as SpleeterRT's STFT wants it.
    const size_t padded = FFTSIZE * (size_t)ceil((double)length / FFTSIZE) + 2 * FFTSIZE;
    float* side[2] = {(float*)calloc(padded, sizeof(float)), (float*)calloc(padded, sizeof(float))};
    if (!side[0] || !side[1]) fail("out of memory");
    for (size_t n = 0; n < length; n++) {
        side[0][FFTSIZE + n] = sound[n * channels];
        side[1][FFTSIZE + n] = sound[n * channels + (channels > 1 ? 1 : 0)];
    }
    drwav_free(sound, NULL);

    OfflineSTFT* st = (OfflineSTFT*)malloc(sizeof(OfflineSTFT));
    if (!st) fail("out of memory");
    InitSTFT(st, (size_t)threads);
    Job job;
    memset(&job, 0, sizeof job);
    job.frames = stft(st, side[0], side[1], padded, &job.re[0], &job.im[0], &job.re[1], &job.im[1]);
    free(side[0]);
    free(side[1]);
    if (!job.frames) fail("nothing to separate");
    progress(8);

    job.model = model;
    job.chunks = (job.frames + FRAMES - 1) / FRAMES;
    job.estimates = calloc(job.chunks, sizeof *job.estimates);
    for (int n = 0; n < STEMS; n++) {
        job.share[n] = (uint16_t*)malloc(job.frames * 2 * BINS * sizeof(uint16_t));
        if (!job.share[n]) fail("out of memory");
    }
    if (!job.estimates) fail("out of memory");
    pthread_mutex_init(&job.lock, NULL);
    pthread_t workers[8];
    for (int n = 1; n < threads; n++)
        if (pthread_create(&workers[n], NULL, work, &job)) fail("cannot start a thread");
    work(&job);
    for (int n = 1; n < threads; n++) pthread_join(workers[n], NULL);
    free(model);
    free(job.estimates);

    // Each stem: the mix's spectrum with the stem's share of every bin, back to sound, to its file.
    float *re[2], *im[2];
    for (int s = 0; s < 2; s++) {
        re[s] = (float*)malloc(job.frames * FFTSIZE * sizeof(float));
        im[s] = (float*)malloc(job.frames * FFTSIZE * sizeof(float));
        if (!re[s] || !im[s]) fail("out of memory");
    }
    float* joined = (float*)malloc((size_t)length * 2 * sizeof(float));
    char* name = (char*)malloc(strlen(argv[3]) + 16);
    if (!joined || !name) fail("out of memory");
    for (int stem = 0; stem < STEMS; stem++) {
        const uint16_t* share = job.share[stem_net[stem]];
        for (size_t t = 0; t < job.frames; t++)
            for (int s = 0; s < 2; s++) {
                const uint16_t* row = share + (t * 2 + (size_t)s) * BINS;
                const float *from_re = job.re[s] + t * FFTSIZE, *from_im = job.im[s] + t * FFTSIZE;
                float *to_re = re[s] + t * FFTSIZE, *to_im = im[s] + t * FFTSIZE;
                float top = 0;
                for (size_t bin = 0; bin < BINS; bin++) {
                    const float gain = (float)row[bin] * (1.0f / 65535.0f);
                    to_re[bin] = from_re[bin] * gain;
                    to_im[bin] = from_im[bin] * gain;
                    if (bin >= BINS - TOP) top += gain;
                }
                top /= TOP; // above 11 kHz: the frame's share of its top quarter below
                for (size_t bin = BINS; bin < HALFWNDLEN; bin++) {
                    to_re[bin] = from_re[bin] * top;
                    to_im[bin] = from_im[bin] * top;
                }
                memset(to_re + HALFWNDLEN, 0, (FFTSIZE - HALFWNDLEN) * sizeof(float));
                memset(to_im + HALFWNDLEN, 0, (FFTSIZE - HALFWNDLEN) * sizeof(float));
            }
        float *left = NULL, *right = NULL;
        const size_t made = istft(st, re[0], im[0], re[1], im[1], job.frames, &left, &right);
        if (!left || !right || made < FFTSIZE + (size_t)length) fail("the sound came back short");
        for (size_t n = 0; n < length; n++) {
            joined[2 * n] = left[FFTSIZE + n];
            joined[2 * n + 1] = right[FFTSIZE + n];
        }
        free(left);
        free(right);
        sprintf(name, "%s%s", argv[3], stem_file[stem]);
        drwav wav;
        drwav_data_format format;
        format.container = drwav_container_riff;
        format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
        format.channels = 2;
        format.sampleRate = RATE;
        format.bitsPerSample = 32;
        if (!drwav_init_file_write(&wav, name, &format, NULL)) fail("cannot write a stem");
        const drwav_uint64 written = drwav_write_pcm_frames(&wav, length, joined);
        drwav_uninit(&wav);
        if (written != length) fail("a stem was cut short");
        progress(87 + 3 * (stem + 1));
    }
    progress(100);
    return 0;
}
