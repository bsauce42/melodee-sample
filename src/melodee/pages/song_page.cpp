// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// SONG (Shift + Pad 12): an arrange view of our own. B1 Export (parity
// phase 6): B2 an audio mixdown, B3 the song as a new pattern. One lane per track,
// time left to right; a clip is a pattern placed on a lane, in the track's
// colour with its notes in small, notches at each repeat. Markers name the
// parts, the brace is the loop, the strip at the bottom is the whole song.
// The wheel moves the cursor by the bar, pads pick the lane. Every clip job
// is on a button of its own, no menu: B3 places the pattern on K2 for the
// length on K3 (on a clip, duplicates it after itself), B2 mutes the clip,
// Shift + B2 its motion, Shift + B3 gives it its own pattern, Push opens its
// notes (on empty time, places as B3 does), Chop splits it at the cursor,
// Erase deletes it, Note Repeat repeats it to the next marker, Loop loops
// its bars. K1 zoom. Minus / Plus jump between markers (or clip edges while
// there are none). Shift + Push adds a marker at the cursor, or names it.
// Play here plays the song from the cursor. Shift + K1 sets the tempo lane at
// the cursor's bar (roadmap P5; under 30 BPM, none there), Shift + K2 its
// shape, a step on its bar or a ramp from the point before; with points the
// lane is a strip over the tracks, its tempo at the cursor beside it.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/new_track.h"
#include "melodee/ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace melodee {
namespace {
using namespace ui;
// Six lanes at a time, tall enough for the clips' names; the window follows
// the selected track.
constexpr unsigned lanes = 6;
constexpr float x0 = 34, x1 = zone::width - zone::pad, lane_top = zone::picture_y + 29, lane_h = 15, lane_gap = 1;
constexpr float strip_h = 14; // roadmap P5: the tempo lane, over the tracks (one track lane fewer while it has points)

unsigned zoom_of(const UiState& ui) { return song_zooms[ui.song_zoom % (sizeof(song_zooms) / sizeof(song_zooms[0]))]; }
// The first bar in view: the remembered one, moved just enough to show the cursor.
unsigned view_of(const UiState& ui) {
    unsigned zoom = zoom_of(ui), view = ui.song_view;
    if (ui.song_cursor < view) view = ui.song_cursor;
    if (ui.song_cursor >= view + zoom) view = ui.song_cursor - zoom + 1;
    return view;
}
juce::String clock_of(const Song& song, unsigned bar, unsigned tempo_tenths, unsigned bar_ticks) { // through the tempo lane (roadmap P5)
    unsigned seconds = unsigned(song.tempo.seconds(bar, double(bar_ticks) / ticks_per_quarter, std::max(tempo_tenths, 300u)) + 0.5);
    char text[16];
    std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
    return text;
}
juce::String dash() { return juce::String::fromUTF8("\xe2\x80\x93"); }

void draw_clip(Paint& p, const Snapshot& s, const Clip& clip, float y, unsigned view, float bw, bool selected) {
    float cx = x0 + (float(clip.bar) - float(view)) * bw, cw = clip.bars * bw;
    float left = std::max(cx, x0), right = std::min(cx + cw, x1);
    if (right <= left + 1) return;
    auto c = track_colour(clip.track);
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(left), int(y), int(right - left + 1), int(lane_h)));
    p.fill(cx + 0.5f, y, cw - 1, lane_h, selected ? c : c.withAlpha(clip.muted ? 0.12f : 0.38f), 2.5f);
    const auto& sketch = s.sketches[clip.track][clip.pattern];
    // Its notes in small: the pattern repeated across the clip.
    // In sixteenths: the sketch keeps the first note of each.
    unsigned bar = s.project.bar_ticks(), length = std::max<unsigned>((sketch.length + sixteenth_ticks - 1) / sixteenth_ticks, 1);
    float step_w = bw * sixteenth_ticks / bar;
    int lo = 127, hi = 0;
    for (unsigned k = 0; k < std::min<unsigned>(length, 64); ++k)
        if (sketch.keys[k]) { lo = std::min<int>(lo, sketch.keys[k]); hi = std::max<int>(hi, sketch.keys[k]); }
    unsigned steps = unsigned(clip.bars * bar / sixteenth_ticks);
    auto note = selected ? colour::bg : c;
    if (hi >= lo && step_w >= 0.6f && !clip.muted)
        for (unsigned k = 0; k < steps; ++k) {
            unsigned at = k % length;
            if (at >= 64 || !sketch.keys[at]) continue;
            float nx = cx + k * step_w, ny = y + lane_h - 3 - (hi > lo ? float(sketch.keys[at] - lo) / (hi - lo) : 0.5f) * (lane_h - 7);
            if (nx > right) break;
            p.fill(nx, ny, std::max(1.0f, step_w * 0.8f), 1, note);
        }
    // A notch at each repeat of the pattern.
    float repeat = length * step_w;
    if (repeat >= 4)
        for (float rx = cx + repeat; rx < cx + cw - 1; rx += repeat) p.fill(rx, y, 0.5f, lane_h, colour::bg);
    p.text("P" + juce::String(clip.pattern + 1), cx + 3, y, 24, lane_h, plain(selected ? colour::bg : c, type::small, Weight::Bold));
    if (selected) p.ring(cx + 0.5f, y, cw - 1, lane_h, colour::text, 2.5f);
    p.g.restoreState();
}

// Freeze plan F3: an audio track's region, a block in the track's colour from where it starts to where it ends (a
// part of a bar where it does not end on one), named by its take; the one at the cursor on the selected lane filled.
void draw_region(Paint& p, const Snapshot& s, const AudioRegion& r, float y, unsigned view, float bw, bool selected) {
    const unsigned bar = std::max(1u, s.project.bar_ticks());
    const float a = float(region_start(r, bar)) / bar, b = float(region_end(r, bar)) / bar;
    float rx = x0 + (a - float(view)) * bw, rw = (b - a) * bw;
    float left = std::max(rx, x0), right = std::min(rx + rw, x1);
    if (right <= left + 1) return;
    auto c = track_colour(r.track);
    p.g.saveState();
    p.g.reduceClipRegion(juce::Rectangle<int>(int(left), int(y), int(right - left + 1), int(lane_h)));
    p.fill(rx + 0.5f, y, rw - 1, lane_h, selected ? c : c.withAlpha(r.muted ? 0.12f : 0.38f), 2.5f);
    if (!r.muted) region_wave(p, s, r, rx + 1, y + 1, rw - 2, lane_h - 2, selected ? colour::bg.withAlpha(0.55f) : c.withAlpha(0.9f));
    if (rw > 22) { // its take's number on a backing of the region's own colour, clear of the wave
        auto style = plain(selected ? colour::bg : c, type::small, Weight::Bold);
        juce::String name = "T" + juce::String(r.take);
        float lw = p.width(name, style) + 5;
        p.fill(rx + 1, y + 1, lw, lane_h - 2, selected ? c : colour::bg.interpolatedWith(c, r.muted ? 0.12f : 0.38f), 2);
        p.text(name, rx + 3, y, lw, lane_h, style);
    }
    if (selected) p.ring(rx + 0.5f, y, rw - 1, lane_h, colour::text, 2.5f);
    p.g.restoreState();
}

// B1-B3 follow what is under the cursor; with Shift, the clip's second jobs. On an audio track's lane, its region's.
SoftKeys keys_of(const Snapshot& s, const Clip* clip, juce::Colour c) {
    if (s.project.tracks[s.ui.track].is_audio()) {
        const unsigned bar = s.project.bar_ticks();
        int here = s.project.song.region_at(s.ui.track, s.ui.song_cursor * bar, bar);
        if (here < 0 || s.ui.shift) return {{"EXPORT", "", ""}, -1, c};
        return {{"EXPORT", s.project.song.regions[size_t(here)].muted ? "UNMUTE" : "MUTE", "DUPLICATE"}, -1, c};
    }
    if (s.ui.shift) return {{"", clip ? "MOTION" : "", clip ? "OWN PATTERN" : ""}, -1, c};
    if (clip) return {{"EXPORT", clip->muted ? "UNMUTE" : "MUTE", "DUPLICATE"}, -1, c};
    const auto& t = s.project.tracks[s.ui.track];
    return {{"EXPORT", "", t.used() && !t.is_audio() ? "PLACE" : ""}, -1, c}; // an audio track's lane holds its takes (freeze plan F3)
}

void draw(Paint& p, const Snapshot& s) {
    const auto& song = s.project.song;
    auto c = colour_of(s);
    unsigned zoom = zoom_of(s.ui), view = view_of(s.ui), cursor = s.ui.song_cursor;
    float bw = (x1 - x0) / zoom;
    int selected_clip = song.clip_at(s.ui.track, cursor);
    Header h = header_of(s);
    h.left.push_back(pill("BAR " + juce::String(cursor + 1) + juce::String::fromUTF8(" \xc2\xb7 ") + clock_of(song, cursor, s.project.tempo, s.project.bar_ticks())));
    if (zoom == song_zooms[0]) h.left.push_back(pill("SNAP 1 BAR"));
    // Following MIDI clock or MTC in, its chip takes the loop's room (the ruler's brace still shows the loop).
    else if (song.looping() && !s.following) h.left.push_back(pill("LOOP " + juce::String(song.loop_start + 1) + dash() + juce::String(song.loop_end), colour::text));
    h.right.push_back(tag_of(s, s.ui.track));

    if (s.ui.song_export) {
        // Export (B1), as on the MPC: B2 an audio mixdown, B3 a new pattern; a tile under each says what it makes.
        // Roadmap P4: the wheel picks what B2 makes: the mixdown as float, 24 or 16 bits, the stems, a MIDI file, or
        // an MPC 3 project.
        static const char* const b2[] {"MIXDOWN", "MIXDOWN", "MIXDOWN", "STEMS", "MIDI FILE", "MPC 3"};
        unsigned kind = s.ui.export_kind % 6;
        SoftKeys keys {{"CANCEL", b2[kind], "PATTERN"}, -1, colour::text, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
        const Icon made = kind == 5 ? Icon::Mpc : kind == 4 ? Icon::Note : kind == 3 ? Icon::Layers : Icon::Wav;
        keys.glyphs = {Icon::None, made, Icon::Pattern};
        frame(p, s, keys, h);
        p.dim_zone(zone::picture_y, zone::picture_h, 0.86f);
        p.dim_zone(zone::knobs_y, zone::knobs_h);
        unsigned bars = song.length(s.project.bar_ticks()), seconds = unsigned(song.tempo.seconds(bars, double(s.project.bar_ticks()) / ticks_per_quarter, std::max(300u, unsigned(s.project.tempo))) + 0.5);
        if (!bars && kind < 4) { // nothing to export yet (a MIDI file and an MPC 3 project take the patterns)
            icon(p, Icon::Pattern, zone::width / 2.0f - 14, zone::picture_y + 44, 28, colour::amber, 1.4f);
            p.text("EMPTY SONG", 0, zone::picture_y + 80, zone::width, 14, caps_text(colour::amber), juce::Justification::centred);
            return;
        }
        unsigned free = pattern_count; // the first pattern number no track uses
        for (unsigned n = 0; n < pattern_count && free == pattern_count; ++n) {
            bool used = false;
            for (unsigned t = 0; t < track_count; ++t) used = used || s.sketches[t][n].notes;
            if (!used) free = n;
        }
        juce::String length = juce::String(seconds / 60) + ":" + juce::String(seconds % 60).paddedLeft('0', 2);
        unsigned stems = 0;
        for (const auto& tr : s.project.tracks) stems += tr.used();
        static const char* const formats[] {"32-BIT FLOAT", "24-BIT DITHER", "16-BIT DITHER", "", "SMF 96 PPQ", "PROJECT"};
        juce::String sub = kind == 3 ? juce::String(stems) + " STEMS" : juce::String(formats[kind]);
        juce::String value = kind == 5 ? juce::String(stems) + (stems == 1 ? " TRACK" : " TRACKS") : bars ? length : juce::String("P");
        key_tiles(p, {KeyTile {}, value_tile(made, value, sub, true), // the wheel picks
                      value_tile(Icon::Pattern, free < pattern_count ? "P" + juce::String(free + 1) : juce::String("FULL"),
                                 juce::String(bars) + (bars == 1 ? " BAR" : " BARS"))});
        return;
    }
    frame(p, s, keys_of(s, selected_clip >= 0 ? &song.clips[size_t(selected_clip)] : nullptr, c), h);

    // Markers, the ruler and the loop brace.
    float label_every = zoom <= 8 ? 1 : zoom / 4.0f;
    for (unsigned m = 0; m < song.marker_count; ++m) {
        const auto& marker = song.markers[m];
        if (marker.bar < view || marker.bar >= view + zoom) continue;
        float mx = x0 + (marker.bar - view) * bw;
        p.text(marker.name, mx + 2, zone::picture_y, 80, 13, caps_text(colour::mid));
        p.fill(mx, zone::picture_y, 0.5f, lane_top - zone::picture_y + lanes * (lane_h + lane_gap), colour::line);
    }
    for (unsigned b = view; b < view + zoom; ++b)
        if (unsigned(b - view) % unsigned(label_every) == 0)
            p.text(juce::String(b + 1), x0 + (b - view) * bw + 2, zone::picture_y + 13, 30, 13, plain(colour::mid, type::small, Weight::SemiBold));
    if (song.looping()) {
        float lx = x0 + (float(song.loop_start) - float(view)) * bw, rx = x0 + (float(song.loop_end) - float(view)) * bw;
        lx = std::max(lx, x0); rx = std::min(rx, x1);
        if (rx > lx) {
            p.fill(lx, lane_top - 3, rx - lx, 1, colour::sec);
            p.fill(lx, lane_top - 4, 1, 3, colour::sec);
            p.fill(rx - 1, lane_top - 4, 1, 3, colour::sec);
        }
    }
    // Roadmap P5: the tempo lane, a strip over the tracks: its line across the view (a step straight up or down on
    // its bar, a ramp sloping from the point before), a dot a point (the cursor's ringed), the tempo at the cursor.
    bool tempo_lane = !song.tempo.empty();
    float tracks_top = lane_top + (tempo_lane ? strip_h + 2 : 0);
    unsigned shown = tempo_lane ? lanes - 1 : lanes;
    if (tempo_lane) {
        const auto& lane = song.tempo;
        double low = lane.at(view, s.project.tempo), high = low; // the tempos in view, at least 10 BPM tall
        auto take = [&](double t) { low = std::min(low, t); high = std::max(high, t); };
        take(lane.at(view + zoom, s.project.tempo));
        for (unsigned n = 0; n < lane.count; ++n)
            if (lane.points[n].bar > view && lane.points[n].bar < view + zoom) { take(lane.points[n].tempo); take(lane.at(lane.points[n].bar - 0.001, s.project.tempo)); }
        if (high - low < 100) { double mid = (high + low) / 2; low = mid - 50; high = mid + 50; }
        float sy = lane_top, sh = strip_h;
        auto y_of = [&](double tempo) { return sy + sh - 2 - float((tempo - low) / (high - low)) * (sh - 4); };
        auto x_of = [&](double bar) { return x0 + float(bar - view) * bw; };
        p.fill(x0, sy, x1 - x0, sh, colour::quiet, 2);
        juce::Path line;
        double now = lane.at(view, s.project.tempo);
        line.startNewSubPath(x0, y_of(now));
        for (unsigned n = 0; n < lane.count; ++n) {
            const auto& pt = lane.points[n];
            if (pt.bar <= view) continue;
            if (pt.bar >= view + zoom) { // past the view: where its ramp has got to at the view's end
                if (pt.ramp) line.lineTo(x1, y_of(lane.at(view + zoom, s.project.tempo)));
                else line.lineTo(x1, y_of(now));
                now = -1;
                break;
            }
            float px = x_of(pt.bar);
            if (pt.ramp) line.lineTo(px, y_of(pt.tempo));
            else { line.lineTo(px, y_of(now)); line.lineTo(px, y_of(pt.tempo)); }
            now = pt.tempo;
        }
        if (now >= 0) line.lineTo(x1, y_of(now));
        p.g.setColour(colour::amber);
        p.g.strokePath(line, juce::PathStrokeType(1.5f));
        for (unsigned n = 0; n < lane.count; ++n) {
            const auto& pt = lane.points[n];
            if (pt.bar < view || pt.bar >= view + zoom) continue;
            float px = x_of(pt.bar), py = y_of(pt.tempo);
            if (pt.bar == cursor) p.ring(px - 3.5f, py - 3.5f, 7, 7, colour::text, 3.5f, 1.2f);
            p.fill(px - 2, py - 2, 4, 4, colour::amber, 2);
        }
        double here = lane.at(cursor, s.project.tempo);
        p.text(juce::String(int(std::lround(here / 10))), zone::pad, sy, 26, sh, plain(colour::amber, type::small, Weight::SemiBold),
               juce::Justification::centred);
    }
    // Lanes: six tracks in view (five under the tempo lane), the selected one among them.
    unsigned first = std::min<unsigned>(s.ui.track >= 2 ? s.ui.track - 2 : 0, track_count - shown);
    for (unsigned row = 0; row < shown; ++row) {
        unsigned t = first + row;
        float y = tracks_top + row * (lane_h + lane_gap);
        auto tc = track_colour(t);
        bool selected = t == s.ui.track;
        if (selected) p.fill(zone::pad, y, 22, lane_h, tc, 3);
        p.text(juce::String(t + 1), zone::pad, y, 22, lane_h, plain(selected ? colour::bg : s.project.tracks[t].used() ? tc : colour::dim, type::small, Weight::Bold),
               juce::Justification::centred);
        for (unsigned n = 0; n < song.count; ++n)
            if (song.clips[n].track == t) draw_clip(p, s, song.clips[n], y, view, bw, int(n) == selected_clip);
        if (s.project.tracks[t].is_audio()) { // freeze plan F3: its regions, the one at the cursor picked
            const int here = selected ? song.region_at(t, cursor * s.project.bar_ticks(), s.project.bar_ticks()) : -1;
            for (unsigned n = 0; n < song.region_count; ++n)
                if (song.regions[n].track == t) draw_region(p, s, song.regions[n], y, view, bw, int(n) == here);
        }
    }
    float bottom = tracks_top + shown * (lane_h + lane_gap);
    p.fill(x0 - 1, lane_top - 2, 0.5f, bottom - lane_top + 2, colour::line);
    // The cursor, and the playhead when the song plays.
    float cx = x0 + (float(cursor) - float(view)) * bw;
    p.fill(cx, zone::picture_y + 14, 1, bottom - zone::picture_y - 14, c);
    p.fill(cx - 2, zone::picture_y + 13, 5, 5, c, 1);
    if (s.song_playing) {
        unsigned bar = s.project.bar_ticks();
        float at = float(s.song_bar) + float(s.ticks % bar) / bar;
        if (at >= view && at < view + zoom) {
            float px = x0 + (at - view) * bw;
            p.fill(px, zone::picture_y + 17, 1, bottom - zone::picture_y - 17, colour::text);
            juce::Path tri;
            tri.addTriangle(px - 3, zone::picture_y + 13, px + 4, zone::picture_y + 13, px + 0.5f, zone::picture_y + 18);
            p.g.setColour(colour::text); p.g.fillPath(tri);
        }
    }
    // The punch lane: punches written into the song, in their target's colour.
    if (song.punch_count) {
        p.text("P", zone::pad, bottom - 3, 22, 11, caps_text(colour::dim), juce::Justification::centred);
        for (unsigned n = 0; n < song.punch_count; ++n) {
            const auto& e = song.punches[n];
            float from = float(e.bar) + e.step / 16.0f, to = from + e.length / 16.0f;
            if (to <= view || from >= view + zoom) continue;
            float px = x0 + (std::max(from, float(view)) - view) * bw, pw = std::max(1.5f, (std::min(to, float(view + zoom)) - std::max(from, float(view))) * bw);
            p.fill(px, bottom, pw, 2.5f, e.target == 255 ? colour::text : track_colour(e.target), 1);
        }
    }
    // The whole song, with the window on it, after its length in bars.
    unsigned total = std::max({song.length(s.project.bar_ticks()), view + zoom, 16u});
    juce::String length = juce::String(song.length(s.project.bar_ticks())) + (song.length(s.project.bar_ticks()) == 1 ? " BAR" : " BARS");
    auto length_style = caps_text(colour::dim);
    float oy = bottom + (song.punch_count ? 13 : 6), lw = p.width(length, length_style) + 1, sx = zone::pad + lw + 6, ow = x1 - sx;
    p.text(length, zone::pad, oy - 5.5f, lw, 11, length_style);
    p.fill(sx, oy, ow, 2, colour::line, 1);
    p.fill(sx + ow * view / total, oy - 1, std::max(4.0f, ow * zoom / total), 4, colour::mid, 2);
}

int clip_index(const Context& c) { return c.project.song.clip_at(c.ui.track, c.ui.song_cursor); }
// Freeze plan F3: the region at the cursor on an audio track's lane, or -1.
int region_index(const Context& c) {
    if (!c.project.tracks[c.ui.track % track_count].is_audio()) return -1;
    const unsigned bar = c.project.bar_ticks();
    return c.project.song.region_at(c.ui.track, c.ui.song_cursor * bar, bar);
}
// The region again right after itself.
void duplicate_region(Context& c, int index) {
    auto& song = c.project.song;
    const unsigned bar = c.project.bar_ticks();
    AudioRegion copy = song.regions[size_t(index)];
    set_region_start(copy, region_end(copy, bar), bar);
    if (!song.place_region(copy, bar)) { c.notice("Song: no room for another region"); return; }
    c.ui.song_cursor = copy.bar;
    c.ui.song_view = uint16_t(view_of(c.ui));
    c.sync_song();
    c.dirty = true;
}

void place(Context& c) {
    if (!c.track().used() || c.track().is_audio()) return;
    Clip clip;
    clip.track = c.ui.track; clip.pattern = c.ui.song_pattern; clip.bar = c.ui.song_cursor; clip.bars = c.ui.song_length;
    if (c.project.song.place(clip)) c.sync_song();
}

void duplicate(Context& c, int index) {
    Clip copy = c.project.song.clips[size_t(index)];
    copy.bar = copy.end();
    if (!c.project.song.place(copy)) return;
    c.ui.song_cursor = copy.bar;
    c.ui.song_view = uint16_t(view_of(c.ui));
    c.sync_song();
}

// A copy of its pattern in the first empty slot, so editing it changes this clip alone.
void own_pattern(Context& c, int index) {
    auto& song = c.project.song;
    Clip clip = song.clips[size_t(index)];
    const auto& patterns = c.patterns.at[clip.track];
    for (unsigned n = 0; n < pattern_count; ++n) {
        bool placed = false;
        for (unsigned k = 0; k < song.count; ++k) placed |= song.clips[k].track == clip.track && song.clips[k].pattern == n;
        if (placed || !patterns[n].empty() || n == clip.pattern) continue;
        c.copy_pattern(clip.track, clip.pattern, n);
        song.clips[size_t(index)].pattern = uint8_t(n);
        c.sync_song();
        return;
    }
    c.notice("No free pattern");
}

// Into the clip's pattern: its notes, or with `motion` its motion lanes.
void open_clip(Context& c, int index, bool motion) {
    Clip clip = c.project.song.clips[size_t(index)];
    c.launch(clip.track, clip.pattern);
    c.sync_song();
    auto engine = c.track().engine;
    c.open(motion ? PageId::Motion : engine == EngineKind::Sampler ? PageId::StepEdit : PageId::Roll);
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down) return;
    c.select_track(pad); // pads pick the lane
    offer_new_track(c);  // an empty one: its doors, then back here
}

void wheel(Context& c, int delta) {
    if (c.ui.song_export) { c.ui.export_kind = uint8_t(std::clamp(int(c.ui.export_kind) + delta, 0, 5)); return; } // what B2 makes (roadmap P4)
    if (c.track().is_audio() && !c.ui.shift) { // freeze plan F3: an audio lane's regions one by one (Shift: by the bar)
        const auto& song = c.project.song;
        const unsigned bar = c.project.bar_ticks();
        std::array<uint16_t, song_regions> lane {};
        unsigned count = song.lane_regions(c.ui.track, bar, lane);
        if (count) {
            int at = -1, here = song.region_at(c.ui.track, c.ui.song_cursor * bar, bar);
            for (unsigned n = 0; n < count; ++n) if (int(lane[n]) == here) at = int(n);
            if (at < 0) { // between regions: the next one on, or the one before
                at = delta > 0 ? int(count) : -1;
                for (unsigned n = 0; n < count; ++n)
                    if (song.regions[lane[n]].bar > c.ui.song_cursor) { at = delta > 0 ? int(n) - 1 : int(n); break; }
            }
            at = std::clamp(at + delta, 0, int(count) - 1);
            c.ui.song_cursor = song.regions[lane[size_t(at)]].bar;
            c.ui.song_view = uint16_t(view_of(c.ui));
            return;
        }
    }
    c.ui.song_cursor = uint16_t(std::clamp<int>(c.ui.song_cursor + delta, 0, int(song_bars) - 1));
    c.ui.song_view = uint16_t(view_of(c.ui));
}

void push(Context& c) {
    if (c.ui.song_export) return;
    if (c.track().is_audio()) { if (region_index(c) >= 0) c.open(PageId::Stage); return; } // freeze plan F3: its CLIP
    int index = clip_index(c);
    if (index >= 0) open_clip(c, index, false);
    else place(c);
}

void soft(Context& c, int key) {
    if (c.ui.song_export) { c.song_export_soft(key); return; } // B1 Cancel, B2 Audio mixdown, B3 New pattern
    int index = clip_index(c);
    if (c.ui.shift) {
        if (index < 0) return;
        if (key == 1) open_clip(c, index, true);
        if (key == 2) own_pattern(c, index);
        return;
    }
    if (key == 0) c.ui.song_export = true; // Export, as on the MPC (Patterns is on Seq)
    if (c.track().is_audio()) { // freeze plan F3: the region at the cursor muted, or again after itself
        int region = region_index(c);
        if (region < 0) return;
        if (key == 1) { auto& r = c.project.song.regions[size_t(region)]; r.muted = !r.muted; c.sync_song(); c.dirty = true; }
        if (key == 2) duplicate_region(c, region);
        return;
    }
    if (key == 1 && index >= 0) {
        auto& clip = c.project.song.clips[size_t(index)];
        clip.muted = !clip.muted;
        c.sync_song();
    }
    if (key == 2) { if (index >= 0) duplicate(c, index); else place(c); }
}

// Minus / Plus: the previous or next marker, or clip edge while there are no markers.
void nudge(Context& c, int way) {
    const auto& song = c.project.song;
    int cursor = c.ui.song_cursor, best = -1;
    auto consider = [&](int bar) {
        if (way > 0 ? bar > cursor && (best < 0 || bar < best) : bar < cursor && (best < 0 || bar > best)) best = bar;
    };
    for (unsigned m = 0; m < song.marker_count; ++m) consider(song.markers[m].bar);
    if (!song.marker_count)
        for (unsigned n = 0; n < song.count; ++n) { consider(song.clips[n].bar); consider(song.clips[n].end()); }
    if (best < 0) best = way > 0 ? cursor : 0;
    c.ui.song_cursor = uint16_t(best);
    c.ui.song_view = uint16_t(view_of(c.ui));
}
} // namespace

// Shift + Push: a marker at the cursor with the next ready name, or on a
// marker the Name sheet to type its name (K1 there still steps the ready ones).
void song_marker(Context& c) {
    if (c.project.song.marker_at(c.ui.song_cursor) >= 0) c.name_marker(c.ui.song_cursor);
    else c.project.song.step_marker(c.ui.song_cursor);
}

// Chop: the clip in two at the cursor (an audio track's region: at the cursor's bar, both parts playing on as before).
void song_chop(Context& c) {
    auto& song = c.project.song;
    if (int region = region_index(c); region >= 0) {
        const unsigned bar = c.project.bar_ticks(), at = c.ui.song_cursor * bar;
        AudioRegion r = song.regions[size_t(region)];
        if (at <= region_start(r, bar)) { c.notice("Chop: cursor inside a region"); return; }
        if (song.region_count >= song.regions.size()) { c.notice("Chop: the song is full"); return; }
        uint32_t head = uint32_t(std::min<uint64_t>(r.frames, ticks_to_frames(at - region_start(r, bar), r.tempo)));
        AudioRegion tail = r;
        tail.offset = r.offset + head; tail.frames = r.frames - head; tail.fade_in = 0;
        set_region_start(tail, at, bar);
        auto& first = song.regions[size_t(region)];
        first.frames = head; first.fade_out = 0;
        if (tail.frames) song.regions[song.region_count++] = tail;
        c.sync_song();
        c.dirty = true;
        return;
    }
    int index = clip_index(c);
    if (index < 0 || c.ui.song_cursor <= song.clips[size_t(index)].bar) { c.notice("Chop: cursor inside a clip"); return; }
    if (song.count >= song.clips.size()) { c.notice("Chop: the song is full"); return; } // the tail would be lost
    Clip clip = song.clips[size_t(index)];
    unsigned cursor = c.ui.song_cursor;
    song.clips[size_t(index)].bars = uint16_t(cursor - clip.bar);
    Clip tail = clip;
    tail.bar = uint16_t(cursor); tail.bars = uint16_t(clip.end() - cursor);
    song.place(tail);
    c.sync_song();
}

// Shift + Erase: the clip under the cursor again after itself, as DUPLICATE.
void song_copy(Context& c) {
    if (int region = region_index(c); region >= 0) { duplicate_region(c, region); return; } // freeze plan F3
    int index = clip_index(c);
    if (index >= 0) duplicate(c, index);
}

void song_erase(Context& c) {
    if (int region = region_index(c); region >= 0) { c.project.song.erase_region(unsigned(region)); c.sync_song(); c.dirty = true; return; } // freeze plan F3
    int index = clip_index(c);
    if (index < 0) return;
    c.project.song.erase(unsigned(index));
    c.sync_song();
}

// Note Repeat: the clip again and again up to the next marker.
void song_repeat(Context& c) {
    auto& song = c.project.song;
    int index = clip_index(c);
    if (index < 0) return;
    Clip clip = song.clips[size_t(index)];
    unsigned until = song.next_marker(clip.bar);
    if (until <= clip.end()) { c.notice("Repeat: needs a marker ahead"); return; }
    for (unsigned at = clip.end(); at < until;) {
        Clip copy = clip;
        copy.bar = uint16_t(at); copy.bars = uint16_t(std::min<unsigned>(clip.bars, until - at));
        if (!song.place(copy)) break;
        at += copy.bars;
    }
    c.sync_song();
}

// Loop: the brace on the clip's bars; again, or on empty time, off.
bool song_loop(Context& c) {
    auto& song = c.project.song;
    if (int region = region_index(c); region >= 0) { // freeze plan F3: the region's bars, a part of one as a whole bar
        const unsigned bar = c.project.bar_ticks();
        const auto& r = song.regions[size_t(region)];
        unsigned from = r.bar, to = (region_end(r, bar) + bar - 1) / bar;
        bool same = song.loop_start == from && song.loop_end == to;
        song.loop_start = uint16_t(same ? 0 : from); song.loop_end = uint16_t(same ? 0 : to);
        c.sync_song();
        return true;
    }
    int index = clip_index(c);
    if (index < 0 && !song.looping()) return false;
    bool same = index >= 0 && song.loop_start == song.clips[size_t(index)].bar && song.loop_end == song.clips[size_t(index)].end();
    if (index < 0 || same) song.loop_start = song.loop_end = 0;
    else { song.loop_start = song.clips[size_t(index)].bar; song.loop_end = song.clips[size_t(index)].end(); }
    c.sync_song();
    return true;
}

const PageSpec song_page {draw, pad, wheel, push, soft, nudge};

} // namespace melodee
