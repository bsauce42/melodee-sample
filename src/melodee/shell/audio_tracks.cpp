// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/audio_tracks.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/stream_pager.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>

namespace melodee {
namespace fs = std::filesystem;

namespace {
constexpr unsigned longest_take_seconds = 6 * 3600; // what a take's file is read to (WavReader stops there)
constexpr unsigned take_bits = 24;                  // as the MPC records its audio tracks
// A take's number in a file name, a take still being written (".part") too.
unsigned take_in(std::string name) {
    const std::string part = ".part";
    if (name.size() > part.size() && !name.compare(name.size() - part.size(), part.size(), part)) name.resize(name.size() - part.size());
    return audio_take_of(name);
}
} // namespace

AudioTracks::AudioTracks(EngineHost& h) : host(h) {
    thread = std::thread([this] { run(); });
}

AudioTracks::~AudioTracks() {
    {
        std::lock_guard<std::mutex> held(lock);
        stopping = true;
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
    if (taking) { take_file.close(); std::error_code e; fs::remove(take_part, e); }
    for (auto& m : made) trash.push_back(m.pack);
    made.clear();
    if (pager) pager->flush();
    for (auto* pack : trash) delete pack;
}

void AudioTracks::build(AudioPackJob job) {
    std::lock_guard<std::mutex> held(lock);
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const AudioPackJob& j) { return j.track == job.track; }), jobs.end());
    jobs.push_back(std::move(job));
    wake.notify_all();
}

bool AudioTracks::take_made(AudioPackMade& out) {
    std::lock_guard<std::mutex> held(lock);
    if (made.empty()) return false;
    out = made.front();
    made.pop_front();
    return true;
}

void AudioTracks::discard(AudioPack* pack) {
    if (!pack) return;
    std::lock_guard<std::mutex> held(lock);
    trash.push_back(pack);
    wake.notify_all();
}

bool AudioTracks::record(const AudioTakeJob& job) {
    std::lock_guard<std::mutex> held(lock);
    if (take_wanted || busy.load()) return false;
    take_job = job;
    take_wanted = true;
    busy.store(true);
    wake.notify_all();
    return true;
}

bool AudioTracks::take_recorded(AudioTakeDone& out) {
    std::lock_guard<std::mutex> held(lock);
    if (recorded.empty()) return false;
    out = std::move(recorded.front());
    recorded.pop_front();
    return true;
}

unsigned AudioTracks::next_take(const std::string& folder, unsigned at_least) {
    unsigned next = std::max(at_least, 1u);
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
        unsigned take = take_in(entry.path().filename().string());
        if (take) next = std::max(next, take + 1);
    }
    return next;
}

unsigned AudioTracks::prune(const std::string& folder, const std::vector<unsigned>& kept) {
    unsigned removed = 0;
    std::error_code error;
    std::vector<fs::path> going;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
        const std::string name = entry.path().filename().string();
        unsigned take = audio_take_of(name);
        if (take && std::find(kept.begin(), kept.end(), take) == kept.end()) going.push_back(entry.path());
    }
    for (const auto& path : going) { std::error_code e; if (fs::remove(path, e)) ++removed; }
    return removed;
}

void AudioTracks::run() {
    for (;;) {
        AudioPackJob job;
        bool have = false;
        std::vector<AudioPack*> going;
        {
            std::unique_lock<std::mutex> held(lock);
            // Often enough to keep up with a take (the engine host's ring holds 10 s at the highest rate).
            wake.wait_for(held, std::chrono::milliseconds(25), [&] { return stopping || !jobs.empty() || !trash.empty() || take_wanted; });
            if (stopping) return;
            if (!jobs.empty()) { job = std::move(jobs.front()); jobs.pop_front(); have = true; }
            going.swap(trash);
        }
        if (!going.empty()) {
            if (pager) pager->flush(); // the pager has seen every close the audio thread sent: none of their pins is taken after
            for (auto* pack : going) delete pack; // their takes' parts go with them
        }
        drain();
        if (have) { make(job); drain(); }
    }
}

// Each region of the job that is not muted, by start: its take read in place (once for every region of it), its
// frames counted at the take's own rate, and a second from where it starts pinned so it plays at once there.
void AudioTracks::make(const AudioPackJob& job) {
    AudioPackMade out;
    out.track = job.track; out.serial = job.serial;
    auto pack = std::make_unique<AudioPack>();
    pack->track = uint8_t(job.track);
    std::vector<AudioRegion> regions = job.regions;
    const unsigned bar = std::max(job.bar_ticks, 1u);
    std::sort(regions.begin(), regions.end(), [&](const AudioRegion& a, const AudioRegion& b) { return region_start(a, bar) < region_start(b, bar); });
    std::map<unsigned, ZonePart*> parts;
    for (const auto& r : regions) {
        if (r.muted || !r.used()) continue;
        auto found = parts.find(r.take);
        ZonePart* part = nullptr;
        if (found != parts.end()) {
            part = found->second;
        } else {
            const std::string path = (fs::path(job.folder) / audio_take_name(r.take)).string();
            WavReader reader;
            auto owned = std::make_unique<ZonePart>();
            if (reader.open(path, longest_take_seconds).empty() && read_in_place(path, reader.layout(), *owned)) {
                owned->frames.store(owned->total);
                part = owned.get();
                pack->takes.push_back(std::move(owned));
            }
            parts[r.take] = part;
        }
        if (!part || !part->total) { ++out.missing; continue; }
        const double k = double(part->rate) / 44100.0; // the region counts 44.1 kHz frames; the take has its own
        auto own = [&](uint32_t frames) { return uint32_t(std::llround(double(frames) * k)); };
        RegionClip c;
        c.take = part;
        c.start = region_start(r, bar);
        c.end = region_end(r, bar);
        c.offset = std::min(own(r.offset), part->total - 1);
        c.frames = std::min(own(r.frames), part->total - c.offset);
        if (!c.frames) continue;
        c.fade_in = std::min(own(r.fade_in), c.frames);
        c.fade_out = std::min(own(r.fade_out), c.frames);
        c.gain = std::pow(10.0f, float(r.gain) / 200.0f);
        c.reverse = r.reverse;
        const uint32_t second = std::min(c.frames, uint32_t(part->rate));
        if (pager) {
            if (c.reverse) pager->hold(*part, c.offset + c.frames - second, second);
            else if (pager->hold(*part, c.offset, second)) c.held = second;
        }
        if (!pack->regions.empty() && pack->regions.back().end > c.start) pack->regions.back().end = c.start; // never two at once
        pack->regions.push_back(c);
    }
    out.pack = pack.release();
    std::lock_guard<std::mutex> held(lock);
    made.push_back(out);
}

// The take: its file opened when it is asked for, the engine host's frames written as they come, and once the take
// is over and every frame is in, the file closed and named. A take stopped before it began leaves no file.
void AudioTracks::drain() {
    auto& ring = host.audio_take();
    if (!taking) {
        bool wanted = false;
        {
            std::lock_guard<std::mutex> held(lock);
            wanted = take_wanted;
        }
        if (!wanted) {
            auto state = ring.state();
            if (state == Bounce::State::Done || state == Bounce::State::Cancelled || state == Bounce::State::Failed) {
                float drop[2 * 1024];
                while (ring.pop(drop, 1024)) {} // nobody asked for it
                ring.release();
            }
            return;
        }
        std::error_code error;
        fs::create_directories(take_job.folder, error);
        take_job.take = next_take(take_job.folder, take_job.take);
        take_part = (fs::path(take_job.folder) / audio_take_name(take_job.take)).string() + ".part";
        take_rate = ring.rate();
        taking = true;
        if (!take_file.open(take_part, take_rate, take_bits)) {
            AudioTakeDone done;
            done.job = take_job; done.error = "cannot write to the card";
            taking = false;
            std::lock_guard<std::mutex> held(lock);
            take_wanted = false;
            recorded.push_back(std::move(done));
            busy.store(false);
            return;
        }
    }
    chunk.resize(2 * 16384);
    for (unsigned n; (n = ring.pop(chunk.data(), 16384)) > 0;) take_file.add(chunk.data(), n);
    auto state = ring.state();
    bool ended = state == Bounce::State::Done || state == Bounce::State::Cancelled || state == Bounce::State::Failed;
    if (!ended || !ring.drained()) return; // a take stopped before its first bar ends Cancelled, with nothing in it
    AudioTakeDone done;
    done.job = take_job;
    done.rate = take_rate;
    done.frames = take_file.frames();
    bool whole = take_file.close();
    ring.release();
    std::error_code error;
    const std::string final_path = take_part.substr(0, take_part.size() - 5);
    if (state == Bounce::State::Failed) done.error = "the card could not keep up";
    else if (!whole) done.error = "cannot write to the card";
    else if (!done.frames) done.error = "nothing recorded";
    if (done.error.empty()) fs::rename(take_part, final_path, error);
    if (!done.error.empty() || error) {
        fs::remove(take_part, error);
        if (done.error.empty()) done.error = "cannot name the take";
    }
    taking = false;
    std::lock_guard<std::mutex> held(lock);
    take_wanted = false;
    recorded.push_back(std::move(done));
    busy.store(false);
}

} // namespace melodee
