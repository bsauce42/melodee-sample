// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/ui_state.h"
#include <algorithm>

// Sequences first. A sequence is one number on every track: pattern n of each (the model's Scene, a column).
// Seq opens the Sequences page (shell/page_stack.h), whose pads launch pattern n on every used track at the next
// bar, as on the stock app. A track with no notes there goes silent, and its empty pattern takes the sequence's
// length, so a take recorded on it fits. Seq held makes the pads the selected track's patterns on any page, as
// the Patterns page's pads are (B1 again): a pad launches that track only, and the track list on Patterns marks a
// track on a pattern of its own. Shift + pad, Seq held or on Patterns, launches the sequence; Shift + pads tapped
// in order while Seq is held chain the sequences: the selected track's chain leads and every track follows it
// (shell/chain_jobs.cpp). Pure, for the shell, the screen and the tests.
namespace melodee {

// Whether Seq held's pads launch sequences now: with Shift held, as Shift + pad launches the sequence. Not while
// patterns freeze (Seq + Sample Record) or a pattern copy waits for its pads.
inline bool sequences_on_pads(const UiState& ui) {
    return ui.seq_held && ui.shift && !ui.seq_print && ui.copying != UiState::Copying::Patterns;
}
// The same for the lights and the pads chip: with Erase held the pads clear the selected track's patterns.
inline bool sequences_lit(const UiState& ui) { return sequences_on_pads(ui) && !ui.erase; }

// The sequence in view: the number the selected track is on.
inline unsigned sequence_in_view(const Project& project, const UiState& ui) {
    return project.tracks[ui.track % track_count].pattern % pattern_count;
}
// A used track on another number than the sequence in view: it plays a pattern of its own.
inline bool own_pattern(const Project& project, const UiState& ui, unsigned t) {
    const auto& track = project.tracks[t % track_count];
    return track.used() && t % track_count != ui.track % track_count && track.pattern % pattern_count != sequence_in_view(project, ui);
}
// How many tracks play a pattern of their own, and the first of them (-1: none).
inline unsigned own_patterns(const Project& project, const UiState& ui, int& first) {
    unsigned count = 0;
    first = -1;
    for (unsigned t = 0; t < track_count; ++t)
        if (own_pattern(project, ui, t)) { if (first < 0) first = int(t); ++count; }
    return count;
}

// Sequence n launched: which tracks go to pattern n. Every used track, with notes there or not.
inline bool sequence_takes(const Track& track) { return track.used(); }
// The length an empty pattern of sequence n takes when the sequence is launched: the selected track's pattern
// there when it has notes, else the longest pattern with notes in the column, else `fallback` (the pattern the
// selected track plays now).
inline uint32_t sequence_length(const Project& project, const PatternBank& patterns, unsigned selected, unsigned n, uint32_t fallback) {
    n %= pattern_count;
    const auto& own = patterns.at[selected % track_count][n];
    if (!own.empty() && own.length) return own.length;
    uint32_t longest = 0;
    for (unsigned t = 0; t < track_count; ++t)
        if (project.tracks[t].used() && !patterns.at[t][n].empty()) longest = std::max(longest, patterns.at[t][n].length);
    return longest ? longest : fallback;
}

} // namespace melodee
