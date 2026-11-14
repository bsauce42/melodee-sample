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
    notice("Sampling " + name);
}

void Context::autosample_tick() {
    if (!printer) return;
    AutoSampleResult r;
    while (printer->take_sampled(r)) {
        autosample_track = -1;
        if (!r.error.empty()) { notice("Auto sample: " + r.error); continue; }
        if (r.project_path != project_path) continue; // another project's: its files stay in Samples/Keygroups
        autosample_landed(r);
    }
}

// The program on the first empty track, a KEYGROUP track with the plug-in track's patterns, mix and name; one undo.
void Context::autosample_landed(const AutoSampleResult& r) {
    const unsigned from = r.track % track_count;
    int target = -1;
    for (unsigned k = 0; k < track_count && target < 0; ++k) if (!project.tracks[k].used()) target = int(k);
    const std::string short_name = fs::path(r.program).parent_path().filename().string();
    if (target < 0) { notice("Saved Keygroups/" + short_name + ", no empty track"); return; }
    const unsigned t = unsigned(target);
    const Track source = project.tracks[from];
    Edit edit = begin_edit("Auto sample", t, -1, false);
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
    made.fx.slots = {}; // its chain, but for plug-in effects (one plays on one track only), the rest moved up
    made.fx.size = source.fx.size;
    unsigned kept = 0;
    for (const auto& fx : source.fx.slots)
        if (fx.used() && fx.kind != plugin_effect_kind && kept < made.fx.slots.size()) made.fx.slots[kept++] = fx;
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
    landing_pads.push_back({uint8_t(t), 0, uint16_t(slot)}); // its envelope and velocity once the program is read
    edit.track_after = std::make_unique<TrackState>(after);
    edits.push(std::move(edit));
    dirty = true;
    notice("Track " + std::to_string(t + 1) + ": " + r.name);
}

} // namespace melodee
