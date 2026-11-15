// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The auto sampler (shell/auto_sample.h): asked for, and what it made put on a
// new KEYGROUP track.
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/print_engine.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/track_copy.h"
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace melodee {
namespace fs = std::filesystem;

namespace {
// A name for files: no character a card or Melodee's paths ("#": a program's pad) keep for themselves.
std::string file_word(const std::string& text) {
    std::string out;
    for (char ch : text) out += std::string("/\\:\"*?<>|#").find(ch) != std::string::npos ? '-' : ch;
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.substr(0, 40);
}
} // namespace

std::string Context::autosample_name(unsigned track) const {
    const auto& tr = project.tracks[track % track_count];
    std::string name;
    if (tr.engine == EngineKind::Plugin && tr.plugin < max_plugins) {
        const auto& ref = project.plugins[tr.plugin];
        std::string preset = ref.preset;
        if (auto cut = preset.find_last_of('/'); cut != std::string::npos) preset = preset.substr(cut + 1);
        if (auto dot = preset.find_last_of('.'); dot != std::string::npos && dot > 0) preset = preset.substr(0, dot);
        name = preset.empty() ? std::string(ref.name) : std::string(ref.name) + " " + preset;
    }
    if (file_word(name).empty()) name = tr.name;
    name = file_word(name);
    return name.empty() ? "Keygroup" : name;
}

void Context::autosample(unsigned track, const AutoSampleSettings& settings) {
    const unsigned t = track % track_count;
    const auto& tr = project.tracks[t];
    if (!printer) return;
    if (tr.engine != EngineKind::Plugin) { notice("Auto sample: plug-in tracks"); printer->note("autosample: track " + std::to_string(t + 1) + " is not a plug-in track"); return; }
    if (deep(t)) { notice("Track " + std::to_string(t + 1) + " is deep frozen"); return; }
    if (printer->busy()) { notice("A print is under way"); printer->note("busy: a print is under way"); return; }
    if (settings.low > settings.high) { notice("LOW is above HIGH"); return; }
    const std::string root = library ? library->root() : std::string();
    if (root.empty()) { notice("No card"); return; }
    // Samples/Keygroups/<name>/, a number after the name when one is there already.
    const fs::path top = fs::path(root) / "Keygroups";
    const std::string base = autosample_name(t);
    std::string name = base;
    std::error_code e;
    for (unsigned n = 2; n < 1000 && fs::exists(top / name, e); ++n) name = base + " " + std::to_string(n);
    PrintJob job;
    job.project_path = project_path;
    job.track = t;
    job.autosample = true;
    job.sampling = settings;
    job.sample_folder = (top / name).string();
    job.sample_name = name;
    job.project = std::make_unique<Project>(project);
    job.bank = std::make_unique<PatternBank>(patterns);
    if (plugins) job.states = plugins->states();
    job.card_top = fs::path(root).parent_path().string();
    if (!printer->start(std::move(job))) { notice("A print is under way"); return; }
    autosample_track = int8_t(t);
    auto_last = settings;
    auto_last.found = {}; // heard again for the next sheet
    notice("Sampling " + name);
}

bool Context::autosample_check(unsigned track, const AutoSampleSettings& settings) {
    const unsigned t = track % track_count;
    if (!printer || project.tracks[t].engine != EngineKind::Plugin || deep(t) || printer->busy()) return false;
    PrintJob job;
    job.project_path = project_path;
    job.track = t;
    job.probing = true;
    job.sampling = settings;
    job.project = std::make_unique<Project>(project);
    job.bank = std::make_unique<PatternBank>(patterns);
    if (plugins) job.states = plugins->states();
    if (library && !library->root().empty()) job.card_top = fs::path(library->root()).parent_path().string();
    return printer->start(std::move(job));
}

void Context::autosample_tick() {
    if (!printer) return;
    AutoProbeResult heard;
    while (printer->take_probed(heard)) {
        auto& sheet = ui.auto_sheet;
        if (!sheet.checking || sheet.track != heard.track) continue; // called off, or another's
        sheet.checking = false;
        if (heard.error.empty()) sheet.settings.found = heard.found;
        else if (heard.error != "stopped") notice("Check: " + heard.error);
    }
    AutoSampleResult r;
    while (printer->take_sampled(r)) {
        autosample_track = -1;
        auto& sheet = ui.auto_sheet;
        const bool shown = sheet.sampling && sheet.track == r.track;
        if (shown) sheet.sampling = false;
        if (!r.error.empty()) {
            notice(r.error == "stopped" ? std::string("Auto sample called off") : "Auto sample: " + r.error);
            continue; // the sheet stays, to try again
        }
        if (r.project_path != project_path) { notice("Saved Keygroups/" + r.name); continue; } // another project's
        const bool watching = shown && ui.page == PageId::AutoSample;
        if (watching) back();
        const int before = first_empty_track();
        autosample_landed(r);
        if (watching && before >= 0 && project.tracks[unsigned(before)].engine == EngineKind::Keygroup) select_track(unsigned(before)); // the new track in hand

    }
}

int Context::first_empty_track() const {
    for (unsigned k = 0; k < track_count; ++k) if (!project.tracks[k].used()) return int(k);
    return -1;
}

// The program on the first empty track, a KEYGROUP track with the plug-in track's patterns, mix and name; one undo.
void Context::autosample_landed(const AutoSampleResult& r) {
    const unsigned from = r.track % track_count;
    const int target = first_empty_track();
    const std::string short_name = fs::path(r.program).parent_path().filename().string();
    if (target < 0) { notice("Saved Keygroups/" + short_name + ", no empty track"); return; }
    const unsigned t = unsigned(target);
    const Track source = project.tracks[from];
    Edit edit = begin_edit("Auto sample", t, -1, true);
    edit.motion_before.reset();
    edit.track_before = std::make_unique<TrackState>(track_state(*this, t));
    load_sound(t, EngineKind::Keygroup, 0);
    int slot = sample_slot(r.program, r.name.substr(0, sizeof(SampleRef::name) - 1));
    if (slot < 0) { restore_track(*this, t, *edit.track_before); notice("Samples full (64)"); return; }
    TrackState after = track_state(*this, t);
    Track& made = after.track;
    made.set_name(r.name.c_str());
    made.mix = source.mix;
    made.mix.mute = false; made.mix.solo = false;
    made.pattern = source.pattern;
    made.speed = source.speed;
    made.chain = source.chain; made.chain_count = source.chain_count; made.chain_at = source.chain_at;
    made.midi_channel = 0; // the plug-in track keeps its MIDI channel
    // Pre FX: its chain, but for plug-in effects (one plays on one track only), the rest moved up. Post FX: the
    // samples hold the chain already, so none.
    made.fx.slots = {};
    made.fx.size = source.fx.size;
    unsigned kept = 0;
    if (!r.post_fx)
        for (const auto& fx : source.fx.slots)
            if (fx.used() && fx.kind != plugin_effect_kind && kept < made.fx.slots.size()) made.fx.slots[kept++] = fx;
    made.bend = source.bend; // the fader's pitch bend, as on the plug-in
    PadSound pad;
    pad.sample = uint16_t(slot);
    pad.play = PadPlay::Gate; pad.decay = 100; pad.sustain = 100; pad.mono = false;
    made.pads = {};
    made.pads[0] = pad;
    made.last_pad = 0; made.bank = 0; made.levels = false; made.levels_pad = 0;
    made.layout = PadLayout::Scale;
    made.last_note = 60;
    after.patterns = patterns.at[from]; // the plug-in track's notes are MIDI notes, as a keygroup's are
    restore_track(*this, t, after);
    // Its Song clips, the same on the keygroup track's lane.
    const unsigned clips = project.song.count;
    unsigned placed = 0;
    for (unsigned c = 0; c < clips; ++c) {
        Clip clip = project.song.clips[c];
        if (clip.track != from) continue;
        clip.track = uint8_t(t);
        project.song.place(clip);
        ++placed;
    }
    if (placed) { sync_song(); edit.song_after = std::make_unique<Song>(project.song); }
    else edit.song_before.reset();
    landing_pads.push_back({uint8_t(t), 0, uint16_t(slot)}); // its envelope and velocity once the program is read
    edit.track_after = std::make_unique<TrackState>(after);
    edits.push(std::move(edit));
    dirty = true;
    std::string said = "Track " + std::to_string(t + 1) + ": " + r.name;
    if (r.loops) said += ", " + std::to_string(r.loops) + " loops";
    notice(said);
}

// ---- the AUTO SAMPLE sheet

void Context::open_auto_sheet() {
    const unsigned t = ui.track % track_count;
    if (project.tracks[t].engine != EngineKind::Plugin) { notice("Auto sample: plug-in tracks"); return; }
    auto& sheet = ui.auto_sheet;
    if (!(sheet.sampling && sheet.track == t)) { // one under way comes back as it is
        const uint8_t row = sheet.row;
        sheet = UiState::AutoSheet {};
        sheet.track = uint8_t(t);
        sheet.settings = auto_last;
        sheet.settings.found = {};
        sheet.row = row;
        // The check: what the plug-in is, heard from a few notes (shell/auto_sample.h AutoFindings).
        sheet.checking = autosample_check(t, sheet.settings);
        if (!sheet.checking && printer && printer->busy()) notice("A print is under way");
    }
    open(PageId::AutoSample);
}

void Context::auto_sheet_go() {
    auto& sheet = ui.auto_sheet;
    if (sheet.sampling || sheet.checking) return;
    if (project.tracks[sheet.track % track_count].engine != EngineKind::Plugin) { notice("Auto sample: plug-in tracks"); return; }
    if (first_empty_track() < 0) notice("No empty track: the program is only saved");
    autosample(sheet.track, sheet.settings);
    sheet.sampling = autosample_track == int8_t(sheet.track);
}

void Context::auto_sheet_cancel() {
    auto& sheet = ui.auto_sheet;
    if (sheet.checking) { // the check called off: its findings, when they come, are dropped
        if (printer && printer->busy()) printer->cancel();
        sheet.checking = false;
        back();
        return;
    }
    if (!sheet.sampling) { back(); return; }
    if (printer && printer->busy() && printer->progress().autosample) printer->cancel(); // its files go with it
    // The sheet stays until the print engine has stopped (autosample_tick()); its result says so.
}

void Context::auto_sheet_row(int way) {
    auto& sheet = ui.auto_sheet;
    if (sheet.sampling || sheet.checking) return;
    sheet.row = uint8_t((sheet.row + 3 + (way < 0 ? 2 : 1)) % 3);
}

void Context::auto_sheet_set(Binding::Kind kind, int value) {
    auto& a = ui.auto_sheet.settings;
    if (ui.auto_sheet.sampling || ui.auto_sheet.checking) return;
    using Kind = Binding::Kind;
    switch (kind) {
    case Kind::AutoLow: a.low = uint8_t(std::clamp(value, 0, 127)); if (a.high < a.low) a.high = a.low; break; // HIGH moves along
    case Kind::AutoHigh: a.high = uint8_t(std::clamp(value, 0, 127)); if (a.low > a.high) a.low = a.high; break;
    case Kind::AutoStep: a.step = uint8_t(std::clamp(value, 1, 12)); break;
    case Kind::AutoLayers: a.layers = uint8_t(std::clamp(value, 1, 4)); break;
    case Kind::AutoHold: a.hold = std::clamp(value, 1, 100) / 10.0f; break;
    case Kind::AutoTail: a.tail = std::clamp(value, 0, 200) / 10.0f; break;
    case Kind::AutoTap: a.post_fx = value != 0; break;
    case Kind::AutoLoop: a.loop_auto = value == 0; a.loop = value == 1; break; // Auto, On, Off
    case Kind::AutoLevel: a.normalize = value != 0; break;
    default: break;
    }
}

} // namespace melodee
