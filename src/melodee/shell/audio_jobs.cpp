// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Freeze plan F3, audio tracks on the MIDI thread: each audio track's regions
// sent to the audio thread as a pack (made on the audio tracks' thread,
// shell/audio_tracks.h) whenever they change, and a take armed, recorded and
// placed on its Song lane, as one edit Undo takes back.
#include "melodee/engine/engine_host.h"
#include "melodee/shell/audio_tracks.h"
#include "melodee/shell/context.h"
#include <algorithm>
#include <string>

namespace melodee {
using Type = Command::Type;

namespace {
std::vector<AudioRegion> lane_of(const Project& project, unsigned t) {
    std::vector<AudioRegion> out;
    if (!project.tracks[t].is_audio()) return out;
    const auto& song = project.song;
    for (unsigned n = 0; n < song.region_count; ++n) if (song.regions[n].track == t) out.push_back(song.regions[n]);
    return out;
}
bool same_lane(const std::vector<AudioRegion>& a, const std::vector<AudioRegion>& b) {
    if (a.size() != b.size()) return false;
    for (size_t n = 0; n < a.size(); ++n) if (!(a[n] == b[n])) return false;
    return true;
}
} // namespace

void Context::sync_audio() {
    if (!printer) return; // the host tests and the monkey: no card, no takes
    if (!audio_tracks) {
        audio_tracks = std::make_shared<AudioTracks>(host);
        audio_tracks->set_pager(&host.streams());
    }
    const std::string folder = audio_folder(project_path);
    const unsigned bar = project.bar_ticks();
    const bool all = project_path != audio_sent_path || bar != audio_sent_bar;
    audio_sent_path = project_path;
    audio_sent_bar = bar;
    for (unsigned t = 0; t < track_count; ++t) {
        auto lane = lane_of(project, t);
        const bool stale = all || ((audio_stale >> t) & 1);
        if (!stale && same_lane(lane, audio_sent[t])) continue;
        const bool had = stale || !audio_sent[t].empty();
        audio_sent[t] = lane;
        ++audio_serial[t];
        if (lane.empty() || folder.empty()) {
            if (had) { Command c {Type::AudioRegions, uint8_t(t)}; c.audio = nullptr; host.send(c); }
            continue;
        }
        audio_tracks->build({t, audio_serial[t], folder, bar, std::move(lane)});
    }
    audio_stale = 0;
}

void Context::audio_tick() {
    // What the Record page shows: the take armed, and whether it runs (it ends on its last bar, or with Stop).
    ui.audio_rec_track = int8_t(audio_take_track);
    ui.audio_rec_running = audio_take_track >= 0 && host.audio_take().running();
    if (!printer) return;
    if (!audio_tracks || audio_stale || project_path != audio_sent_path) sync_audio();
    AudioPack* done = nullptr;
    while (host.take_done_audio(done)) audio_tracks->discard(done);
    AudioPackMade made;
    while (audio_tracks->take_made(made)) {
        const unsigned t = made.track % track_count;
        if (made.serial != audio_serial[t] || !project.tracks[t].is_audio()) { audio_tracks->discard(made.pack); continue; }
        if (!made.peaks.empty()) add_peaks(std::move(made.peaks));
        Command c {Type::AudioRegions, uint8_t(t)};
        c.audio = made.pack;
        if (!host.send(c)) { audio_tracks->discard(made.pack); audio_stale |= 1u << t; continue; } // again next pass
        if (made.missing)
            notice("Track " + std::to_string(t + 1) + ": " + std::to_string(made.missing) + (made.missing == 1 ? " take missing" : " takes missing"));
    }
    AudioTakeDone take;
    while (audio_tracks->take_recorded(take)) {
        audio_take_track = -1;
        const unsigned t = take.job.track % track_count;
        if (!take.error.empty()) {
            if (take.error != "nothing recorded") notice("Audio: " + take.error);
            continue;
        }
        // Another project since (its file stays in the old one's folder), or the track is no longer an audio track.
        if (take.job.folder != audio_folder(project_path) || !project.tracks[t].is_audio()) continue;
        if (!take.peaks.empty()) { TakePeaks one; one[take.job.take] = std::move(take.peaks); add_peaks(std::move(one)); }
        AudioRegion r;
        r.track = uint8_t(t);
        r.take = uint16_t(take.job.take);
        r.bar = uint16_t(take.job.from_bar);
        r.frames = r.take_frames = uint32_t((uint64_t(take.frames) * 44100 + take.rate / 2) / std::max(1u, take.rate));
        r.tempo = take.job.tempo;
        if (!r.frames) continue;
        Edit edit = begin_edit("Record audio", t, -1, true);
        if (!project.song.place_region(r, project.bar_ticks())) notice("Song: no room for another region");
        else notice("Track " + std::to_string(t + 1) + ": take " + std::to_string(r.take));
        sync_song();
        commit_edit(std::move(edit));
        dirty = true;
    }
}

void Context::add_peaks(TakePeaks&& more) {
    auto next = std::make_shared<TakePeaks>(audio_peaks ? *audio_peaks : TakePeaks {});
    for (auto& [take, peaks] : more) (*next)[take] = std::move(peaks);
    audio_peaks = std::move(next);
}

bool Context::record_audio(unsigned track, unsigned from, unsigned to) {
    const unsigned t = track % track_count;
    if (!project.tracks[t].is_audio()) return false;
    if (!printer) { notice("Audio: no card"); return false; }
    if (project_path.empty()) { notice("Audio: save the project first"); return false; }
    sync_audio();
    if (audio_take_track >= 0 || audio_tracks->writing()) { notice("Audio: the last take is still being written"); return false; }
    AudioTakeJob job;
    job.folder = audio_folder(project_path);
    job.take = project.song.last_take() + 1; // at least; the thread takes the next free number in the folder
    job.track = t;
    job.from_bar = from;
    job.to_bar = to;
    job.tempo = project.tempo;
    if (!audio_tracks->record(job)) { notice("Audio: the last take is still being written"); return false; }
    if (!host.send({Type::AudioRecord, uint8_t(t), 0, uint16_t(from), int16_t(to)})) {
        notice("Audio: try again");
        return false;
    }
    audio_take_track = int(t);
    ui.audio_rec_track = int8_t(t);
    ui.audio_rec_from = uint16_t(from);
    ui.audio_rec_to = uint16_t(to);
    return true;
}

void Context::cancel_audio() {
    if (audio_take_track < 0) return;
    host.send({Type::AudioRecord, uint8_t(audio_take_track), 0, 0, -1});
}

// Play on an audio track's Record page: one pass of the brace's bars (B3: from the cursor until Stop), the song
// played from the take's first bar, counted in when Count-In is on. Playing already: nothing (Stop first).
void Context::audio_record_play() {
    if (host.playing()) return;
    const auto& song = project.song;
    const bool loop = !ui.audio_free && song.looping();
    const unsigned from = loop ? song.loop_start : ui.song_cursor, to = loop ? song.loop_end : 0;
    if (!record_audio(ui.track, from, to)) return;
    played_song = true;
    ui.span_count = 0;
    send({Type::Play, uint8_t(ui.settings.count_in), 0, uint16_t(from), 1});
}

// The wheel there: the brace a bar at a time, Shift its length (at least a bar); without B3's loop the cursor. With
// no brace yet, the first turn puts one of four bars at the cursor.
void Context::audio_record_wheel(int delta) {
    if (!delta || (audio_take_track >= 0 && host.playing())) return;
    auto& song = project.song;
    if (ui.audio_free) {
        ui.song_cursor = uint16_t(std::clamp<int>(int(ui.song_cursor) + delta, 0, int(song_bars) - 1));
        return;
    }
    if (!song.looping()) {
        song.loop_start = ui.song_cursor;
        song.loop_end = uint16_t(std::min<unsigned>(ui.song_cursor + 4u, song_bars));
    } else if (ui.shift) {
        song.loop_end = uint16_t(std::clamp<int>(int(song.loop_end) + delta, int(song.loop_start) + 1, int(song_bars)));
    } else {
        const int length = int(song.loop_end) - int(song.loop_start);
        const int start = std::clamp<int>(int(song.loop_start) + delta, 0, int(song_bars) - length);
        song.loop_start = uint16_t(start);
        song.loop_end = uint16_t(start + length);
    }
    ui.song_cursor = song.loop_start;
    send({Type::SongLoop, 0, 0, song.loop_start, int16_t(song.loop_end)});
    dirty = true;
}

void Context::toggle_audio_free() {
    if (audio_take_track >= 0 && host.playing()) return;
    auto& song = project.song;
    const bool looping = !ui.audio_free && song.looping();
    ui.audio_free = looping;
    if (!looping && !song.looping()) { // a brace of four bars at the cursor
        song.loop_start = ui.song_cursor;
        song.loop_end = uint16_t(std::min<unsigned>(ui.song_cursor + 4u, song_bars));
        send({Type::SongLoop, 0, 0, song.loop_start, int16_t(song.loop_end)});
        dirty = true;
    }
    notice(ui.audio_free ? "Records from the cursor until Stop" : "Records the loop once");
}

// Stage's cards on the region at the Song cursor. Start and end trim the take (the audio keeps its place in time: a
// later start moves the region on by as much); what it then covers on the lane is its alone.
void Context::set_region_value(const Binding& b, int value) {
    using K = Binding::Kind;
    auto& song = project.song;
    if (b.id < 0 || unsigned(b.id) >= song.region_count) return;
    const unsigned bar = project.bar_ticks();
    AudioRegion r = song.regions[size_t(b.id)];
    auto frames = [](int ms) { return uint32_t(uint64_t(std::max(ms, 0)) * 441 / 10); };
    bool placed = false; // its span on the lane changed: placed again
    switch (b.kind) {
    case K::RegionGain: r.gain = int16_t(std::clamp(value, region_gain_min, region_gain_max)); break;
    case K::RegionStart: {
        const uint32_t end = r.offset + r.frames;
        const uint32_t start = std::min(frames(value), end - 1);
        const int64_t moved = int64_t(frames_to_ticks(start, r.tempo)) - int64_t(frames_to_ticks(r.offset, r.tempo));
        const int64_t at = std::max<int64_t>(0, int64_t(region_start(r, bar)) + moved);
        r.offset = start; r.frames = end - start;
        set_region_start(r, uint32_t(at), bar);
        placed = true;
        break;
    }
    case K::RegionEnd: r.frames = std::clamp<uint32_t>(frames(value), r.offset + 1, r.take_frames) - r.offset; placed = true; break;
    case K::RegionFadeIn: r.fade_in = std::min(frames(value), r.frames); break;
    case K::RegionFadeOut: r.fade_out = std::min(frames(value), r.frames); break;
    case K::RegionReverse: r.reverse = value != 0; break;
    case K::RegionBpm: r.tempo = uint16_t(std::clamp(value, 300, 3000)); placed = true; break;
    case K::RegionSemi: r.semi = int8_t(std::clamp(value, -24, 24)); break;
    case K::RegionFine: r.fine = int8_t(std::clamp(value, -50, 50)); break;
    case K::RegionWarp: r.warp = value != 0; break;
    case K::RegionMode: r.mode = WarpMode(std::clamp(value, 0, int(WarpMode::Count) - 1)); break;
    default: return;
    }
    r.fade_in = std::min(r.fade_in, r.frames); r.fade_out = std::min(r.fade_out, r.frames);
    if (placed) {
        song.erase_region(unsigned(b.id));
        song.place_region(r, bar);
        ui.song_cursor = r.bar; // still on it
    } else {
        song.regions[size_t(b.id)] = r;
    }
    sync_song();
    dirty = true;
}

} // namespace melodee
