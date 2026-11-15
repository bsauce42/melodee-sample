// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"

// The pads chip. When the pads do not play the selected track's sound as it is, the header's first chip names
// what they do (TRACKS, PATTERNS, SEQUENCES, STEPS, PAD FX, LOOPS, LEVELS), and the button of that job is bright
// (shell/lights.cpp). It is a sign only: no button gets a new press. Sample opens the sound's page, where the
// pads play. "" while they play, while a sheet draws its own pads (the MENU, a question, a name, Exit), while a
// copy, a move or a capture asks for its pads (its prompt has the header), and in mute mode, whose own chip says
// pads or tracks (pages/overlays.cpp). Pure, for the header and the tests.
namespace melodee {

inline const char* pad_job(const Project& project, const UiState& ui) {
    if (ui.menu || ui.ask != UiState::Ask::None || ui.name.open() || ui.exit_open) return "";
    if (ui.copying != UiState::Copying::Off || ui.scene_capture) return ""; // a copy, a move or a capture: its prompt says what to pick
    if (ui.pad_bank) return "TRACKS";                                           // Pad Bank held
    if (ui.loop_held && ui.page != PageId::Looper) return "LOOPS";              // Loop held: the loop grid
    if (ui.seq_held) return "PATTERNS";                                         // Seq held: the selected track's patterns
    if (ui.mute_mode()) return "";
    switch (ui.page) {
    case PageId::Tracks: return "TRACKS";
    case PageId::Patterns: return "PATTERNS";
    case PageId::Scenes: return "SEQUENCES";
    case PageId::StepEdit: if (ui.step_pads) return "STEPS"; break;
    case PageId::Punch: return "PAD FX";
    case PageId::Looper: return "LOOPS";
    default: break;
    }
    const auto& t = project.tracks[ui.track % track_count];
    if (t.levels || (t.engine != EngineKind::Sampler && t.layout == PadLayout::Levels)) return "LEVELS"; // 16 Levels on
    return "";
}

} // namespace melodee
