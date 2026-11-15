// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// AUTO SAMPLE (shell/auto_sample.h): Resample (Shift + Pad 11) on a plug-in
// track. The keys from C-1 to G9 as a strip, the range sampled in the
// track's colour, each root a mark over it. Beside: how many samples, the
// keys, what they take on the card at most. K1-K3 a row of three, B2 the
// next: KEYS (LOW, HIGH, STEP), NOTE (LAYERS, HOLD, TAIL), SOUND (TAP, LOOP,
// LEVEL). B3 or Push SAMPLE: the roots light up as they are done, the one
// playing blinks; under the keys its note and velocity, the note drawn as it
// records (hold, then release), the speed, the time left and the job's bar
// with a light running along it; B1 calls it off and its files go. The pads
// play the plug-in to try it; Minus / Plus their octave. Shift + Pad 11 again
// closes it. Done, it goes back and the new KEYGROUP track is in hand.
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include <cmath>

namespace melodee {
using namespace ui;

namespace {
const char* const rows[] {"KEYS", "NOTE", "SOUND"};

void block(Paint& p, float x, float y, float w, const juce::String& caps, const juce::String& value, const juce::String& unit,
           juce::Colour value_colour) {
    p.text(caps, x, y, w, 12, caps_text(colour::mid));
    float vw = p.text(value, x, y + 13, w, 24, plain(value_colour, type::value, Weight::SemiBold));
    if (unit.isNotEmpty()) p.text(unit, x + vw + 5, y + 21, w - vw - 5, 14, p.fit(unit, plain(colour::sec, type::body), w - vw - 5, type::floor));
}

juce::String clock(float seconds) {
    int s = int(std::lround(std::max(0.0f, seconds)));
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

bool black(unsigned key) { const unsigned n = key % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }

void draw(Paint& p, const Snapshot& s) {
    const auto& sheet = s.ui.auto_sheet;
    const auto& a = sheet.settings;
    const unsigned t = sheet.track % track_count;
    const auto c = track_colour(t);
    const auto& job = s.print;
    const bool sampling = sheet.sampling, ours = sampling && job.busy && job.autosample && job.track == t;
    const auto roots = a.roots();
    const unsigned layers = std::clamp<unsigned>(a.layers, 1, 4), samples = unsigned(roots.size()) * layers;

    Header h = header_of(s);
    if (sampling) h.left.push_back(pill(Icon::Keyboard, ours ? "SAMPLING " + juce::String(std::min<unsigned>(job.through + 1, job.count)) + "/" + juce::String(job.count)
                                                            : juce::String("SAMPLING"), colour::ice));
    else h.left.push_back(chip(Icon::Keyboard, "AUTO SAMPLE", colour::ice));
    h.right.push_back(tag_of(s, t));
    SoftKeys keys {{sampling ? "CANCEL" : "BACK", sampling ? "" : rows[sheet.row % 3], sampling ? "" : "SAMPLE"}, -1, c,
                   {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    if (!sampling) {
        keys.dots[1] = 3; keys.dot_at[1] = uint8_t(sheet.row % 3);
        keys.glyphs[2] = Icon::Rec;
        keys.tints[2] = a.low <= a.high ? colour::rec : colour::dim;
    }
    frame(p, s, keys, h);

    // The keys, 0-127 across the width: white and black, the range in the track's colour, a mark a root.
    const float x0 = zone::pad, kw = (zone::width - 2.0f * zone::pad) / 128.0f, ky = zone::picture_y + 18, kh = 30;
    for (unsigned k = 0; k < 128; ++k) {
        const bool in = k >= a.low && k <= a.high;
        juce::Colour fill = black(k) ? colour::quiet : colour::line;
        if (in) fill = black(k) ? c.withAlpha(0.45f) : c.withAlpha(0.8f);
        p.fill(x0 + k * kw, ky + (black(k) ? 0 : 0), std::max(1.0f, kw - 0.4f), black(k) ? kh * 0.62f : kh, fill, 0);
    }
    const unsigned now_key = ours && job.pattern >= 0 ? unsigned(job.pattern) : 128;
    for (uint8_t r : roots) {
        const float rx = x0 + (r + 0.5f) * kw;
        const bool done = ours && ((job.done[r / 64 % job.done.size()] >> (r % 64)) & 1);
        const bool playing = r == now_key;
        juce::Colour mark = sampling ? (done ? colour::ice : playing ? colour::text : colour::dim) : colour::text;
        if (playing && (s.now / 250) % 2) mark = c;
        p.fill(rx - 1.5f, ky - 9, 3, 7, mark, 1.5f);
    }
    // Octave names under the keys: C-1, C1, C3 ... (every second C, 11 px).
    for (int oct = 0; oct <= 10; oct += 2) {
        const unsigned k = unsigned(oct * 12);
        p.text(juce::String(note_name(int(k))), x0 + k * kw, ky + kh + 3, 34, 12, plain(colour::mid, type::small));
    }

    // Under them: what it makes, or how it goes.
    const float y = ky + kh + 20, w = (zone::width - 2.0f * zone::pad) / 3;
    if (sampling) {
        // Left: the note and its velocity. Right: the note as it records, hold then release, a bar a slice of time,
        // the line at its end where it is now. Under them the speed and time left, and the whole job's bar.
        const juce::String dash = juce::String::fromUTF8("\xe2\x80\x94");
        const bool live = ours && job.pattern >= 0;
        block(p, x0, y, 80, "NOTE", live ? juce::String(note_name(job.pattern)) : dash, "", colour::text);
        if (live && job.velocity) p.text("VEL " + juce::String(int(job.velocity)), x0, y + 40, 80, 12, caps_text(colour::mid));
        const float bx = x0 + 88, bw = zone::width - zone::pad - bx, by = y - 2, bh = 70;
        p.fill(bx, by, bw, bh, colour::panel, 5);
        const unsigned count = live ? std::min<unsigned>(job.wave_count, unsigned(job.wave.size())) : 0;
        const unsigned hold = std::max<unsigned>(1, job.wave_hold);
        // The bars across: the hold on 60% at first; a longer release pushes the hold left.
        const float span = std::max({float(count), hold / 0.6f, 1.0f});
        const float ix = bx + 4, iw = bw - 8, cy = by + 14 + (bh - 18) / 2, amp = (bh - 22) / 2;
        const float split = ix + iw * std::min(1.0f, hold / span);
        p.text("HOLD", ix, by + 3, std::max(0.0f, split - ix - 2), 12, caps_text(colour::dim));
        if (ix + iw - split > 50) p.text("RELEASE", split + 4, by + 3, ix + iw - split - 4, 12, caps_text(colour::dim));
        p.fill(split, by + 4, 1, bh - 8, colour::line);
        const float step = iw / span;
        for (unsigned b = 0; b < count; ++b) {
            const float a = std::max(0.04f, job.wave[b] / 255.0f) * amp;
            const juce::Colour bar = b < hold ? c : c.withAlpha(0.5f);
            p.fill(ix + b * step, cy - a, std::max(1.0f, step - 0.6f), 2 * a, bar, 0);
        }
        if (live) p.fill(std::min(ix + count * step, ix + iw - 1), by + 4, 1.5f, bh - 8, colour::text);
        // Speed and time left on one line, then the job's bar with a light running along what is done.
        const float fy = by + bh + 6;
        float vw = p.text(ours && job.times_real > 0 ? juce::String::fromUTF8("\xc3\x97") + juce::String(job.times_real, 1) : dash,
                          x0, fy, 70, 16, plain(colour::ice, type::list, Weight::SemiBold));
        p.text("SPEED", x0 + vw + 6, fy, 60, 16, caps_text(colour::mid));
        const juce::String left = ours && job.through ? clock(job.seconds_left) : dash;
        const float lw = p.width("LEFT", caps_text(colour::mid));
        p.text("LEFT", zone::width - zone::pad - lw, fy, lw, 16, caps_text(colour::mid));
        p.text(left, zone::width - zone::pad - lw - 66, fy, 60, 16, plain(colour::text, type::list, Weight::SemiBold), juce::Justification::centredRight);
        const float gy = fy + 22, gw = zone::width - 2.0f * zone::pad;
        p.fill(x0, gy, gw, 6, colour::quiet, 3);
        const float done = ours && job.count ? std::clamp((float(job.through) + job.fraction) / float(job.count), 0.0f, 1.0f) : 0;
        if (done > 0) {
            const float dw = gw * done;
            p.fill(x0, gy, dw, 6, colour::ice, 3);
            const float run = dw + 30, at = std::fmod(float(s.now % 100000) * 0.12f, run) - 15; // 120 px a second
            for (int k = -12; k <= 12; ++k) {
                const float sx = x0 + at + k;
                if (sx > x0 + 1 && sx < x0 + dw - 2) p.fill(sx, gy + 1, 1, 4, juce::Colours::white.withAlpha(0.8f * (1.0f - std::abs(k) / 12.0f)));
            }
        }
    } else {
        block(p, x0, y, w, "SAMPLES", juce::String(samples), layers > 1 ? juce::String(roots.size()) + juce::String::fromUTF8(" \xc3\x97 ") + juce::String(layers) : juce::String(), colour::text);
        block(p, x0 + w, y, w, "KEYS", juce::String(note_name(a.low)) + juce::String::fromUTF8("\xe2\x80\x93") + juce::String(note_name(a.high)), "", colour::text);
        // At most: each note its hold and its whole tail, 24-bit stereo at the device's rate.
        const double mb = double(samples) * (a.hold + a.tail) * std::max(1u, s.audio_rate) * 6.0 / (1024.0 * 1024.0);
        block(p, x0 + 2 * w, y, w, "CARD", juce::String::fromUTF8("\xe2\x89\xa4 ") + (mb >= 10 ? juce::String(int(std::lround(mb))) : juce::String(mb, 1)), "MB", colour::text);
    }
    knob_strip(p, cards_of(s)); // the row of three (shell/bindings.cpp)
}

void soft(Context& c, int key) {
    if (key == 0) c.auto_sheet_cancel();
    if (key == 1) c.auto_sheet_row(1);
    if (key == 2) c.auto_sheet_go();
}

void push(Context& c) { c.auto_sheet_go(); }
void wheel(Context& c, int delta) { if (delta) c.auto_sheet_row(delta > 0 ? 1 : -1); } // the wheel walks the rows too
} // namespace

const PageSpec auto_sample_page {draw, play_pad, wheel, push, soft, octave};

} // namespace melodee
