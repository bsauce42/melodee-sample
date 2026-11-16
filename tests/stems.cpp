// SPDX-License-Identifier: AGPL-3.0-only
// Stems (src/melodee/shell/stems.h) with stand-ins for the separating program: shell scripts that take its
// arguments, print its lines and write its three files. A job copies the program into its work folder, gives it the
// sample as a WAV file, writes what it made to the paths asked for at the sample's length and leaves nothing else
// behind; a job cancelled, a program that fails, one that makes too little and one that is not there each say so
// and leave no file.
#include "melodee/shell/stems.h"
#include "melodee/engine/sample_data.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace melodee;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

std::string script(const fs::path& folder, const char* name, const char* body) {
    const auto path = folder / name;
    std::ofstream(path) << "#!/bin/sh\n" << body;
    fs::permissions(path, fs::perms::owner_all);
    return path.string();
}

constexpr uint32_t frames = 88200; // 2 s

StemsJob job(const fs::path& folder, const std::string& engine, const char* name, uint32_t length = frames) {
    StemsJob j;
    j.engine = engine;
    j.work = (folder / "work").string();
    j.left.resize(length);
    j.right.resize(length);
    for (uint32_t n = 0; n < length; ++n) {
        j.left[n] = 0.5f * std::sin(float(n) * 0.05f);
        j.right[n] = 0.25f * std::sin(float(n) * 0.031f);
    }
    for (unsigned n = 0; n < stem_count; ++n) j.paths[n] = (folder / "Stems" / (std::string(name) + " " + stem_name(n) + ".wav")).string();
    return j;
}

// The job's result, waited for at most `seconds`.
bool finished(StemSeparator& separator, StemsResult& out, unsigned seconds) {
    for (unsigned n = 0; n < seconds * 100; ++n) {
        if (separator.take(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool none_left(const StemsResult& r) {
    for (const auto& path : r.paths) if (fs::exists(path)) return false;
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path folder = argv[1];
    // The program's arguments are threads, time step, bin limit, stems and the file; it prints a line a step.
    const char* lines = "[ \"$4\" = 3 ] || exit 9\n"
                        "echo 'Audio & model file and sample rate conversion takes: 0.1 sec'\n"
                        "echo 'Inference neural networks using 3 cores takes 0.1 sec'\n";
    const auto good = script(folder, "good.sh", (std::string(lines) + "cp \"$5\" \"$5_Drum.wav\" && cp \"$5\" \"$5_Vocal.wav\" && cp \"$5\" \"$5_Accompaniment.wav\"\n").c_str());
    const auto slow = script(folder, "slow.sh", (std::string(lines) + "sleep 30\n").c_str());
    const auto failing = script(folder, "failing.sh", "echo 'Invalid audio sample rate / frame count'\nexit 3\n");
    const auto half = script(folder, "half.sh", (std::string(lines) + "cp \"$5\" \"$5_Drum.wav\"\n").c_str());

    check(stems_memory(60 * 44100) > uint64_t(768) << 20, "a minute's job counts the 682 MB measured and its files in RAM");
    check(stems_memory(60 * 44100) > stems_memory(10 * 44100), "a longer sample takes more memory");
    check(stems_seconds(60 * 44100) >= 50 && stems_seconds(13 * 44100) >= 17, "the times are no shorter than those measured");

    StemSeparator separator;
    StemsResult result;
    {
        check(separator.start(job(folder, good, "LOOP")), "a job starts");
        check(!separator.start(job(folder, good, "OTHER")), "one job at a time");
        check(finished(separator, result, 20), "the job ends");
        check(result.error.empty(), "the job has no error");
        check(!separator.busy() && separator.progress().fraction > 0.999f, "it is done, all of it");
        for (unsigned n = 0; n < stem_count; ++n) {
            SampleData stem;
            check(read_wav(result.paths[n], stem).empty(), "a stem's file reads");
            check(stem.frames == frames, "a stem is as long as the sample, to the frame");
            bool same = stem.frames == frames;
            for (uint32_t f = 0; same && f < frames; f += 97)
                same = std::fabs(stem.left[f] - 0.5f * std::sin(float(f) * 0.05f)) < 1e-4f && std::fabs(stem.right[f] - 0.25f * std::sin(float(f) * 0.031f)) < 1e-4f;
            check(same, "a stem holds what the program made, each side its own");
        }
        check(fs::path(result.paths[0]).filename() == "LOOP DRUMS.wav" && fs::path(result.paths[2]).filename() == "LOOP MUSIC.wav", "the files are the ones asked for");
        check(!fs::exists(folder / "work"), "the work folder is gone");
    }
    {
        check(separator.start(job(folder, slow, "CUT")), "a second job starts once the first is taken");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(separator.busy() && separator.progress().busy, "it runs");
        separator.cancel();
        check(finished(separator, result, 5), "a cancelled job ends at once");
        check(result.error == "stopped", "a cancelled job says stopped");
        check(none_left(result) && !fs::exists(folder / "work"), "a cancelled job leaves nothing");
    }
    {
        check(separator.start(job(folder, failing, "BAD")), "a job with a failing program starts");
        check(finished(separator, result, 20) && result.error == "program failed", "a failing program is named");
        check(none_left(result) && !fs::exists(folder / "work"), "a failed job leaves nothing");
    }
    {
        check(separator.start(job(folder, half, "HALF")), "a job with a program that makes one file starts");
        check(finished(separator, result, 20) && result.error == "no VOCALS made", "the missing stem is named");
        check(none_left(result), "the stem that was written goes too");
    }
    {
        check(separator.start(job(folder, (folder / "none").string(), "NONE")), "a job without the program starts");
        check(finished(separator, result, 20) && result.error == "no program on the card", "the missing program is named");
        check(separator.start(job(folder, good, "TINY", stems_least_frames - 1)), "a job with too short a sample starts");
        check(finished(separator, result, 20) && result.error == "too short", "too short a sample is refused");
    }
    if (failures) return 1;
    std::printf("stems ok\n");
    return 0;
}
