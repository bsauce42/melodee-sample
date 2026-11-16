// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// The entry of the stem separator Melodee runs beside itself (tools/stems/build.py):
// SpleeterRT's offline program, unchanged, with what a background job on the MPC
// needs set up before it starts. SpleeterRT's own main is compiled as
// spleeter_main (-Dmain=spleeter_main).
//
//   melodee-stems <threads> <timeStep> <binLimit> <stems> <file.wav>
//
// It writes <file.wav>_Drum.wav, _Vocal.wav and _Accompaniment.wav (44.1 kHz,
// 32-bit float, stereo) beside the input.
#define _GNU_SOURCE
#include <libgen.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

int spleeter_main(int argc, char* argv[]);

// SpleeterRT calls this for a threaded OpenBLAS; the single-threaded build has none.
__attribute__((weak)) void openblas_set_num_threads(int threads) { (void)threads; }

int main(int argc, char* argv[]) {
    // It ends with Melodee: nothing is left running, and holding memory, after a crash or a restart.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) return 4;
    // Melodee's own open files (the sound card, the screen) are none of its business.
    for (int fd = 3; fd < 1024; fd++) close(fd);
    // A line per stage as it ends, so Melodee can show how far the job is; a pipe would hold them back to the end.
    setvbuf(stdout, NULL, _IOLBF, 0);
    // Below everything Melodee runs: the scheduling class and priority are inherited from the thread that started it.
    struct sched_param normal = {0};
    sched_setscheduler(0, SCHED_OTHER, &normal);
    setpriority(PRIO_PROCESS, 0, 19);
    // The cores it may use, as a hexadecimal mask (MELODEE_STEMS_CPUS=7: cores 0 to 2).
    const char* cpus = getenv("MELODEE_STEMS_CPUS");
    if (cpus && *cpus) {
        unsigned long mask = strtoul(cpus, NULL, 16);
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int cpu = 0; cpu < 64; cpu++)
            if (mask >> cpu & 1) CPU_SET(cpu, &set);
        if (CPU_COUNT(&set) > 0) sched_setaffinity(0, sizeof set, &set);
    }
    // Short of memory, the kernel ends this program, not Melodee.
    FILE* oom = fopen("/proc/self/oom_score_adj", "w");
    if (oom) {
        fputs("1000\n", oom);
        fclose(oom);
    }
    // SpleeterRT names its outputs after the input and writes them in the working folder.
    if (argc >= 6) {
        char* copy = strdup(argv[5]);
        if (copy && chdir(dirname(copy)) != 0) {
            fprintf(stderr, "melodee-stems: cannot enter the folder of %s\n", argv[5]);
            return 3;
        }
        free(copy);
    }
    return spleeter_main(argc, argv);
}
