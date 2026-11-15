// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// RECORD (Sample Record on a sampler track), the MPC's Sample Record Mode:
// tap the pad to record onto; the take starts when the level passes the
// threshold (at once at -96 dB; with rec length Seq while playing, where the
// pattern comes round). During the take the other pads tapped cut it there,
// each getting the part from its cut on. The pad again, Stop or Push ends
// it and Record stays; Sample Record ends it and goes back to the pad's
// Sample page. K1 source, K2 rec length, K3 threshold, B1 monitor, as on the
// MPC and in step with Input Config (B2 opens it); Shift + K3 gain. Push on
// its own records onto the first empty pad. Takes are saved to
// Samples/Recorded on the card. With Melodee's own sound as the source (a
// track, Resample) the pads play the instrument from the tap until the take
// ends (Context::record_keys(); the shell routes them before this page).
#include "melodee/engine/take_recorder.h"
#include "melodee/pages/common.h"
#include "melodee/pages/input_meter.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/context.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;
using State = TakeRecorder::State;

// Freeze plan F3, the Record page on an audio track (model/audio_track.h): the song's lanes around it with the loop
// brace (or the cursor, B3) where the take goes, the input's level, and what one Play records. K1 the track's source,
// K2 its monitor, K3 the input's gain.
constexpr float lane_x0 = 34, lane_x1 = zone::width - zone::pad, ruler_y = zone::picture_y, lanes_y = zone::picture_y + 30;
constexpr float lane_h = 15, lane_gap = 1;
constexpr unsigned lane_count = 4;
juce::String dash() { return juce::String::fromUTF8("\xe2\x80\x93"); }
juce::String dot() { return juce::String::fromUTF8(" \xc2\xb7 "); }

void draw_audio(Paint& p, const Snapshot& s) {
    const auto& song = s.project.song;
    const auto& tr = s.track();
    const unsigned t = s.ui.track % track_count, bar = std::max(1u, s.project.bar_ticks());
    auto c = colour_of(s);
    const bool armed = s.ui.audio_rec_track == int(t);
    const bool running = armed && s.ui.audio_rec_running;
    const bool loop = armed ? s.ui.audio_rec_to != 0 : !s.ui.audio_free && song.looping();
    const unsigned from = armed ? s.ui.audio_rec_from : loop ? song.loop_start : s.ui.song_cursor;
    const unsigned to = armed ? s.ui.audio_rec_to : loop ? song.loop_end : 0;
    Header h = header_of(s);
    h.recording = running;
    juce::String where = to ? juce::String(from + 1) + dash() + juce::String(to) : "FROM BAR " + juce::String(from + 1);
    if (running) h.left.push_back(chip("REC " + (to ? where : "FROM " + juce::String(from + 1)), colour::rec));
    else h.left.push_back(pill(to ? "LOOP " + where : where, colour::amber));
    h.left.push_back(pill(source_icon(tr.audio.source), juce::String(audio_source_label(tr.audio.source)).toUpperCase()));
    if (s.speaker_quieted) h.left.push_back(pill(Icon::SpeakerOff, "", colour::amber));
    h.right.push_back(tag_of(s, t));
    SoftKeys keys {{"MONITOR", "INPUT", running ? "" : loop ? "NO LOOP" : "LOOP"}, -1, c}; // B3 the other way
    keys.toggles = 1;
    keys.lit = tr.audio.monitor != TrackMonitor::Off ? 1 : 0;
    keys.leds[0] = tr.audio.monitor == TrackMonitor::Auto ? colour::amber : colour::play;
    keys.glyphs[1] = source_icon(tr.audio.source);
    frame(p, s, keys, h);

    // The view: sixteen bars from the bar before the take's first, or on to show where the song plays.
    const unsigned zoom = 16;
    unsigned view = from ? from - 1 : 0;
    if (s.song_playing && (s.song_bar < view || s.song_bar >= view + zoom)) view = s.song_bar >= 1 ? s.song_bar - 1 : 0;
    const float bw = (lane_x1 - lane_x0) / zoom;
    auto x_of = [&](double b) { return lane_x0 + float(b - double(view)) * bw; };
    for (unsigned m = 0; m < song.marker_count; ++m) { // markers, then the ruler's bar numbers
        const auto& marker = song.markers[m];
        if (marker.bar < view || marker.bar >= view + zoom) continue;
        p.text(marker.name, x_of(marker.bar) + 2, ruler_y, 80, 13, caps_text(colour::mid));
        p.fill(x_of(marker.bar), ruler_y, 0.5f, lanes_y - ruler_y + lane_count * (lane_h + lane_gap), colour::line);
    }
    for (unsigned b = view; b < view + zoom; ++b) // every fourth bar from the take's first
        if ((b + 4 * zoom - from) % 4 == 0) p.text(juce::String(b + 1), x_of(b) + 2, ruler_y + 13, 30, 13, plain(colour::mid, type::small, Weight::SemiBold));
    // The take's bars: the brace over the lanes and a wash down them (to the right edge when it runs until Stop).
    const juce::Colour mark = running ? colour::rec : colour::amber;
    {
        float lx = std::max(x_of(from), lane_x0), rx = to ? std::min(x_of(to), lane_x1) : lane_x1;
        if (rx > lx) {
            p.fill(lx, lanes_y - 4, rx - lx, 1.5f, mark);
            p.fill(lx, lanes_y - 5, 1.5f, 4, mark);
            if (to) p.fill(rx - 1.5f, lanes_y - 5, 1.5f, 4, mark);
            p.fill(lx, lanes_y - 2, rx - lx, lane_count * (lane_h + lane_gap), mark.withAlpha(0.08f));
        }
    }
    // Four lanes, the audio track among them: other tracks' clips as plain bars, regions as blocks named by take.
    unsigned first = std::min<unsigned>(t >= 1 ? t - 1 : 0, track_count - lane_count);
    for (unsigned row = 0; row < lane_count; ++row) {
        const unsigned lt = first + row;
        const float y = lanes_y + row * (lane_h + lane_gap);
        auto tc = track_colour(lt);
        const bool mine = lt == t;
        if (mine) p.fill(zone::pad, y, 22, lane_h, tc, 3);
        p.text(juce::String(lt + 1), zone::pad, y, 22, lane_h, plain(mine ? colour::bg : s.project.tracks[lt].used() ? tc : colour::dim, type::small, Weight::Bold),
               juce::Justification::centred);
        p.g.saveState();
        p.g.reduceClipRegion(juce::Rectangle<int>(int(lane_x0), int(y), int(lane_x1 - lane_x0), int(lane_h)));
        for (unsigned n = 0; n < song.count; ++n) {
            const auto& clip = song.clips[n];
            if (clip.track != lt) continue;
            p.fill(x_of(clip.bar) + 0.5f, y + 4, clip.bars * bw - 1, lane_h - 8, tc.withAlpha(clip.muted ? 0.12f : 0.3f), 2);
        }
        for (unsigned n = 0; n < song.region_count; ++n) {
            const auto& r = song.regions[n];
            if (r.track != lt) continue;
            const double a = double(region_start(r, bar)) / bar, b = double(region_end(r, bar)) / bar;
            float rx = x_of(a), rw = float(b - a) * bw;
            p.fill(rx + 0.5f, y, rw - 1, lane_h, tc.withAlpha(r.muted ? 0.12f : 0.38f), 2.5f);
            if (!r.muted) region_wave(p, s, r, rx + 1, y + 1, rw - 2, lane_h - 2, tc.withAlpha(0.9f));
            if (rw > 26) { // its take's number on a backing, clear of the wave
                auto style = plain(tc, type::small, Weight::Bold);
                juce::String name = "T" + juce::String(r.take);
                float lw = p.width(name, style) + 5;
                p.fill(rx + 1, y + 1, lw, lane_h - 2, colour::bg.interpolatedWith(tc, r.muted ? 0.12f : 0.38f), 2);
                p.text(name, rx + 3, y, lw, lane_h, style);
            }
        }
        if (mine && running && s.song_playing) { // the take growing, in red, from its first bar to where the song is
            double now = double(s.song_bar) + double(s.ticks % bar) / bar;
            if (now > from) p.fill(x_of(from) + 0.5f, y, float(now - from) * bw, lane_h, colour::rec.withAlpha(0.8f), 2.5f);
        }
        p.g.restoreState();
    }
    const float bottom = lanes_y + lane_count * (lane_h + lane_gap);
    // Where the song plays, or (stopped) where the take starts.
    double at = s.song_playing ? double(s.song_bar) + double(s.ticks % bar) / bar : double(from);
    if (at >= view && at < view + zoom) {
        float px = x_of(at);
        p.fill(px, ruler_y + 14, 1, bottom - ruler_y - 14, s.song_playing ? colour::text : mark);
    }
    // The input's level, left over right.
    const float mx = zone::pad, mw = zone::width - 2 * zone::pad, my = bottom + 6;
    auto fraction = [](float level) { return level > 0 ? std::clamp((20 * std::log10(level) + 96) / 96, 0.0f, 1.0f) : 0.0f; };
    for (int side = 0; side < 2; ++side) {
        float level = fraction(side ? s.take.right : s.take.left), sy = my + side * 4.5f;
        p.fill(mx, sy, mw, 3, colour::line, 1.5f);
        if (level > 0) p.fill(mx, sy, mw * level, 3, c, 1.5f);
    }
    // What one Play records.
    juce::String what;
    if (running) what = to ? "STOPS AT BAR " + juce::String(to + 1) : juce::String("UNTIL STOP");
    else if (to) what = juce::String(to - from) + (to - from == 1 ? " BAR" : " BARS") + dot() + "ONE PASS";
    else what = "UNTIL STOP";
    auto style = plain(running ? colour::rec : colour::amber, type::small, Weight::Bold);
    float ww = p.width(what, style);
    p.text(what, zone::width - zone::pad - ww, my + 12, ww + 1, 14, style);
}

void draw(Paint& p, const Snapshot& s) {
    if (s.track().is_audio()) { draw_audio(p, s); return; }
    auto c = colour_of(s);
    auto state = State(s.take.state);
    const auto& in = s.ui.settings.input;
    Header h = header_of(s);
    h.recording = state == State::Recording;
    bool taking = s.ui.rec_pad >= 0 && s.ui.rec_track == s.ui.track;
    char label[4];
    if (taking) { pad_label(unsigned(s.ui.rec_pad), label); h.left.push_back(pill(Icon::Pads, juce::String(label), c)); }
    h.left.push_back(pill(source_icon(in.source), juce::String(in.source == InputSource::Track ? "T" + juce::String(in.track % track_count + 1)
                                                                                             : juce::String(input_source_name(in.source))).toUpperCase()));
    if (s.speaker_quieted) h.left.push_back(pill(Icon::SpeakerOff, "", colour::amber));
    h.right.push_back(tag_of(s, s.ui.track));
    bool kit = s.track().engine == EngineKind::Sampler;
    SoftKeys keys {{"MONITOR", "INPUT", kit ? "PRINT" : ""}, -1, c}; // B3 PRINT (freeze plan F2c): the kit's PRINT sheet
    if (kit) keys.glyphs[2] = Icon::Snow;
    keys.toggles = 1;
    keys.lit = in.monitor != Monitor::Off ? 1 : 0;
    keys.leds[0] = in.monitor == Monitor::Auto ? colour::amber : colour::play;
    keys.glyphs[1] = source_icon(in.source);
    frame(p, s, keys, h);

    float x = zone::pad, w = zone::width - 2 * zone::pad;
    // At the corner the state as a glyph (ready, waiting for the level or the loop, recording, saving) and the time so far.
    Icon glyph = state == State::Armed ? Icon::Ring : state == State::Cued ? Icon::Loop : state == State::Recording ? Icon::Rec
               : state == State::Done || (taking && state == State::Idle) ? Icon::Check : Icon::Pads;
    juce::Colour tone = state == State::Recording ? colour::rec : state == State::Armed || state == State::Cued ? colour::amber : colour::mid;
    double bar_frames = double(s.project.bar_ticks()) / ticks_per_quarter * 60.0 / (s.tempo / 10.0) * 44100;
    unsigned bars = rec_bars(in.length);
    bool going = state == State::Recording || state == State::Done;
    juce::String elapsed = going ? juce::String(s.take.frames / 44100.0, 1) + " s" : in.length == rec_seq ? juce::String("SEQ") : juce::String();
    auto time_style = plain(colour::text, type::body, Weight::SemiBold);
    float tw = p.width(elapsed, time_style);
    p.text(elapsed, x + w - tw, zone::picture_y + 1, tw + 1, 16, time_style);
    icon(p, glyph, x + w - tw - (tw > 0 ? 18 : 12), zone::picture_y + 3, 12, tone, 1.6f);

    // The take in its frame: with a length in bars, that many bars numbered (the one being taken lit), so what is
    // left shows; a take without one (Free, up to 60 s) grows across the frame, an eighth of it at least.
    float y = zone::picture_y + 18, wh = 62, mid = y + wh / 2;
    p.fill(x, y, w, wh, colour::panel, 3);
    uint32_t length = std::max<uint32_t>(s.take.length, 1);
    unsigned shown = TakeRecorder::columns;
    if (length >= TakeRecorder::max_frames)
        shown = std::clamp<unsigned>(unsigned((uint64_t(s.take.frames) * TakeRecorder::columns + length - 1) / length), TakeRecorder::columns / 8,
                                     TakeRecorder::columns);
    float span = float(length) * shown / TakeRecorder::columns; // the frames across the picture
    unsigned frame_bars = going && length < TakeRecorder::max_frames && bar_frames > 0 ? std::max(1u, unsigned(length / bar_frames + 0.5)) : bars;
    unsigned now_bar = going && bar_frames > 0 ? unsigned(s.take.frames / bar_frames) : 0;
    if (frame_bars > 1 && frame_bars <= 16)
        for (unsigned b = 0; b < frame_bars; ++b) {
            float bx = x + w * b / frame_bars;
            if (b) p.fill(bx, y, 1, wh, colour::line);
            bool lit = going && b == now_bar;
            if (bx + 20 > x + w - tw - 22) continue; // clear of the time at the corner
            p.text(juce::String(b + 1), bx + 3, zone::picture_y + 1, 24, 16, plain(lit ? colour::text : colour::dim, type::small, Weight::SemiBold));
        }
    float cw = w / shown;
    for (unsigned n = 0; n < shown; ++n) {
        float half = std::max(0.5f, s.take.peaks[n] / 255.0f * (wh / 2 - 4));
        p.fill(x + n * cw, mid - half, std::max(1.0f, cw * 0.6f), half * 2, c);
    }
    if (taking)
        for (unsigned n = 0; n < s.ui.rec_chops; ++n) { // the cuts the pads made, each with its pad as a chip
            float at = x + w * std::min(1.0f, float(s.ui.rec_chop_at[n]) / span);
            p.fill(at, y, 1.2f, wh, colour::text.withAlpha(0.8f));
            pad_label(s.ui.rec_chop_pad[n], label);
            auto style = caps_text(colour::text);
            float lw = p.width(juce::String(label), style) + 6;
            p.fill(at + 3, y + 4, lw, 13, colour::raised, 3);
            p.text(juce::String(label), at + 3, y + 4, lw, 13, style, juce::Justification::centred);
        }
    if (state == State::Recording) { // where it has come to, and what is left of the frame
        float at = x + w * std::min(1.0f, float(s.take.frames) / span);
        p.fill(at - 1, y - 1, 2, wh + 2, colour::rec);
        for (float d = at + 4; d < x + w - 2; d += 6) p.fill(d, mid - 0.6f, 2, 1.2f, colour::dim);
    }
    // The input level, with the marker it has to pass.
    input_meter(p, s, c, x, y + wh + 4, w);
    // What the pads do now, as glyph pairs: a pad (the take's outlined, any other filled) and what it does.
    struct Pair { int pad; Icon does; }; // pad: 1 the take's pad, 2 another pad, 3 the sixteen pads, 0 none (Stop)
    std::array<Pair, 2> pairs {{{2, Icon::Rec}, {0, Icon::None}}};
    size_t count = 1;
    if (record_keys_shown(s)) pairs[0] = {3, Icon::Note};
    else if (state == State::Recording) { pairs = {{{1, Icon::Stop}, {2, Icon::Scissors}}}; count = 2; }
    else if (state == State::Armed || state == State::Cued) { pairs = {{{1, Icon::Play}, {0, Icon::Cross}}}; count = 2; }
    float total = 0;
    for (size_t n = 0; n < count; ++n) total += 12 + 6 + 12 + (n ? 20 : 0);
    float lx = zone::width / 2.0f - total / 2, ly = zone::picture_y + 130;
    for (size_t n = 0; n < count; ++n) {
        if (n) { p.circle(lx + 10, ly + 6, 1.3f, colour::dim); lx += 20; }
        const auto& pr = pairs[n];
        if (pr.pad == 1) p.ring(lx, ly, 12, 12, c, 2.5f, 1.6f);
        else if (pr.pad == 2) p.fill(lx, ly, 12, 12, colour::raised, 2.5f);
        else if (pr.pad == 3) icon(p, Icon::Pads, lx, ly, 12, colour::sec, 1.5f);
        else icon(p, Icon::Stop, lx, ly, 12, colour::sec);
        icon(p, pr.does, lx + 18, ly, 12, pr.does == Icon::Rec ? colour::rec : colour::sec, 1.5f);
        lx += 30;
    }
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down || c.track().engine != EngineKind::Sampler) return; // an audio track's pads play nothing (freeze plan F3)
    if (c.ui.erase) { c.notice("Erase a pad on Pads or Trim"); return; } // here the pads record; nothing is erased by accident
    if (c.frozen_edit(c.ui.track)) return; // freeze plan F2c: a take onto a frozen kit asks first
    c.record_pad(c.track().bank % sampler_banks * 16 + pad);
}
void soft(Context& c, int key) {
    if (c.track().is_audio()) { // freeze plan F3: B1 the track's monitor, B2 Input Config, B3 the loop or the cursor
        auto& a = c.track().audio;
        if (key == 0) {
            a.monitor = TrackMonitor((unsigned(a.monitor) + 1) % unsigned(TrackMonitor::Count));
            c.dirty = true;
            c.notice(std::string("Monitor ") + track_monitor_name(a.monitor));
        }
        if (key == 1) c.open(PageId::InputConfig);
        if (key == 2) c.toggle_audio_free();
        return;
    }
    if (key == 0) c.cycle_monitor();
    if (key == 1) c.open(PageId::InputConfig);
    if (key == 2 && c.track().engine == EngineKind::Sampler && !c.host_recording()) c.open_print_sheet(); // freeze plan F2c
}
void push(Context& c) {
    if (c.track().is_audio()) { if (c.host.playing()) c.stop(); else c.audio_record_play(); return; } // freeze plan F3
    if (!c.frozen_edit(c.ui.track)) c.record_press();
}
void wheel(Context& c, int delta) { if (c.track().is_audio()) c.audio_record_wheel(delta); } // freeze plan F3
} // namespace

const PageSpec record_page {draw, pad, wheel, push, soft, [](Context&, int) {}};

} // namespace melodee
