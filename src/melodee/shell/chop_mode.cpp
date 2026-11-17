// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Chop's pads (parity phase 5): a pad plays its slice and picks it for K1 /
// K2. In Manual, armed (K3 onto Manual, or Push), pad 1 blinks red; tapped,
// it plays the whole region, and the red walks: the pad of the slice the next
// tap makes blinks red, and tapped it cuts the region there (the other pads
// wait; a lit one plays again from its slice, the cuts after it gone). After
// pad 16 the pads turn to bank B and its pad 1 is red, up to D (64 slices).
// Stopped short of the end, the red stays on the next pad: tapped, it plays
// on from the last cut; played to the end, no red. A take is one undo step
// (open_take / end_take). Each cut is pulled onto the strongest hit just before it,
// since a tap comes a little after the sound it means; with Zero Snap on (B2,
// kept with the settings) it then lands on a zero crossing.
#include "melodee/shell/context.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/mpc_wav_slices.h"
#include "melodee/model/slice_banks.h"
#include "melodee/shell/zoom_view.h"
#include <thread>

namespace melodee {

void Context::chop_tap(unsigned pad, uint8_t velocity) {
    auto& sl = ui.slices;
    uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
    if (!frames || !sl.count) return;
    auto audition = [&](uint32_t from, uint32_t to) {
        Command a {Command::Type::Audition, ui.track, 0, 0, int16_t(velocity)};
        a.pad.sample = ui.chop_sample; a.pad.start = from; a.pad.end = to;
        send(a);
    };
    const uint32_t region_end = sl.end(sl.count - 1, frames);
    // A take plays on from slice n (a lit pad tapped, the red one after a take): what comes after n goes.
    auto play_on_from = [&](unsigned n) {
        for (unsigned k = sl.count; k-- > n + 1;) { ui.chop_hand.unpin(sl.start[k]); sl.remove(k); }
        ui.chop_slice = uint8_t(n); ui.chop_more = false;
        ui.chop_take_from = sl.start[n]; ui.chop_take_at = now ? now : 1;
        audition(sl.start[n], region_end);
    };
    if (chop_take_playing(project, ui, now)) { // Manual's take
        const unsigned slice = pad_slice(chop_bank_slice(project, ui, now), pad);
        if (slice < sl.count) { play_on_from(slice); return; } // a lit pad: again from its slice, the cuts after it gone
        if (!red_slice(slice, sl.count, max_slices)) return;    // past the red: nothing while it plays
        uint32_t at = take_frame(ui.chop_take_from, ui.chop_take_at, now); // the red pad: a cut where the region is now
        if (library) {
            auto loaded = library->slot(ui.chop_sample);
            at = zero_cut(snap_to_hit(at, loaded.hits, loaded.hit_count));
        }
        if (sl.add(at, frames, max_slices)) {
            ui.chop_hand.pin(at); // a cut by hand: it stays when the type changes
            for (unsigned n = 0; n < sl.count; ++n) if (sl.start[n] == at) ui.chop_slice = uint8_t(n);
        }
        return;
    }
    if (ui.chop_take_at) end_take(); // it has played out: the pads play slices again (no red: it reached the end)
    if (chop_armed(ui) && pad == 0) { // pad 1, red: the take begins
        open_take();
        chop_unfollow(ui); ui.chop_focus = 0; ui.chop_heard_at = 0;
        if (sl.count) sl.whole(sl.start[0], sl.stop[sl.count - 1]); // from one slice, the whole region
        ui.chop_hand.clear();                                          // and its cuts are the hand cuts from here
        play_on_from(0);
        return;
    }
    const unsigned slice = pad_slice(chop_bank_slice(project, ui, now), pad); // the pad's slice in the bank in view
    if (ui.chop_more && ui.chop_mode == ChopMode::Manual && red_slice(slice, sl.count, max_slices)) { // the red, after a take
        open_take();
        chop_unfollow(ui); ui.chop_focus = 0; ui.chop_heard_at = 0;
        play_on_from(sl.count - 1); // on from the last cut; the red walks again
        return;
    }
    if (slice >= sl.count) return;
    if (slice != ui.chop_slice) { // another slice: the wheel moves its start, and the view opens out again
        ui.chop_fine = false; ui.chop_focus = 0;
        chop_unfollow(ui);
    }
    ui.chop_slice = uint8_t(slice);
    ui.chop_heard_at = 0; // the tap plays it
    audition(sl.start[slice], sl.end(slice, frames));
}

// A take is one undo step, from pad 1 (or the red pad after a take) until it plays out or anything else is done.
void Context::open_take() {
    end_take();
    chop_take_edit = scope_edit(UndoScope {UndoScope::Kind::Chop, uint8_t(ui.track % track_count), 0}, "Chop take");
}
void Context::end_take(uint32_t ms) {
    if (ui.chop_take_at) {
        // Stopped short of the region's end (another control, the page left): the sound stops and the red stays on the
        // next pad, to play on from the last cut. Played to the end, every cut is made: no red.
        const bool short_of_end = chop_take_playing(project, ui, ms ? ms : now);
        ui.chop_more = short_of_end && ui.chop_mode == ChopMode::Manual && ui.slices.count < max_slices;
        if (short_of_end) {
            Command stop {Command::Type::Audition, ui.track, 0, 0, 0};
            stop.pad.sample = no_sample; // the audition voice stops
            send(stop);
        }
    }
    ui.chop_take_at = 0;
    if (!chop_take_edit) return;
    auto edit = std::move(chop_take_edit);
    if (finish_scope(*edit)) { edits.push(std::move(*edit)); dirty = true; }
}

// MIDI thread, from the lights' pass. A take that has played out (or whose page was left) ends: one undo step (the
// red kept on the next pad only when it stopped short of the end). A cut moved by the wheel or K1 / K2 is heard once it has been still for chop_heard_ms,
// with what leads into it: its start from 300 ms before, into the slice to its end; its end, the slice's last second.
// Once (each move puts it off again; nothing repeats).
void Context::chop_tick(uint32_t ms) {
    if (ui.chop_take_at && (ui.page != PageId::Chop || !chop_take_playing(project, ui, ms))) end_take(ms);
    if (!ui.chop_heard_at || int32_t(ms - ui.chop_heard_at) < 0) return;
    ui.chop_heard_at = 0;
    const auto& sl = ui.slices;
    const uint32_t frames = chop_frames(project, ui);
    if (ui.page != PageId::Chop || !frames || ui.chop_slice >= sl.count) return;
    const unsigned n = ui.chop_slice;
    const uint32_t a = sl.start[n], b = sl.end(n, frames);
    Command c {Command::Type::Audition, ui.track, 0, 0, 100};
    c.pad.sample = ui.chop_sample;
    c.pad.start = ui.chop_focus ? std::max(a, b > heard_tail ? b - heard_tail : 0) : (a > heard_lead ? a - heard_lead : 0);
    c.pad.end = b;
    send(c);
}

// SHIFT + the wheel on Chop: the cut the wheel moves (the slice's start, or its end after K2) onto the next hit the way
// it turns, a hit a detent (shell/chop.h next_hit).
void Context::chop_jump(int delta) {
    const auto& sl = ui.slices;
    const uint32_t frames = chop_frames(project, ui);
    if (!delta || !library || !frames || ui.chop_slice >= sl.count) return;
    const auto loaded = library->slot(ui.chop_sample);
    const uint8_t floor = mark_floor(ui.chop_amount[size_t(ChopMode::Threshold)]);
    const unsigned n = ui.chop_slice;
    const bool end = ui.chop_focus;
    // Its room: a start between the cut before and its own end, an end between its start and the cut after, each a
    // shortest slice (20 ms) clear, so a jump never lands on a neighbour's own hit and leaves a sliver.
    const uint32_t a = sl.start[n], b = sl.end(n, frames);
    const uint32_t lo = end ? a + min_slice : (n ? (sl.touches(n - 1, frames) ? sl.start[n - 1] : sl.end(n - 1, frames)) + min_slice : 0);
    const uint32_t hi = end ? (sl.touches(n, frames) ? sl.end(n + 1, frames) : n + 1 < sl.count ? sl.start[n + 1] : frames) - min_slice
                            : (b > min_slice ? b - min_slice : 0);
    uint32_t at = end ? b : a;
    for (int k = 0; k < std::abs(delta); ++k) {
        const uint32_t next = next_hit_within(at, delta, loaded.hits, loaded.hit_count, floor, lo, hi);
        if (next == at) break; // no hit left in its room that way
        at = next;
    }
    if (at == (end ? b : a)) return;
    apply(Binding {end ? Binding::Kind::ChopEnd : Binding::Kind::ChopStart, int32_t(n), 0, int(frames), 0}, int(at));
    ui.chop_view = {}; // a jump goes further than the view: it closes in round the cut where it landed
}

// The slices Chop holds as its sample's chop, written into the sample's WAV file when they changed (as the MPC
// keeps them): before the region changes, and at a save.
void Context::keep_chop() {
    if (ui.chop_sample < sample_slots && ui.slices.count) {
        const auto& kept = project.chops[ui.chop_sample];
        SampleChop now;
        now.from = ui.chop_from; now.to = ui.chop_to; now.mode = ui.chop_mode;
        now.amount = int16_t(ui.chop_amount[size_t(ui.chop_mode) % ui.chop_amount.size()]);
        now.slices = ui.slices;
        now.hand = ui.chop_hand;
        const bool same = kept.from == now.from && kept.to == now.to && kept.mode == now.mode && kept.amount == now.amount
                       && kept.slices.same(now.slices);
        if (!same || !kept.hand.same(now.hand)) {
            now.unwritten = kept.unwritten || !same; // the hand cuts alone are not in the file: nothing to write
            project.chops.edit(ui.chop_sample) = now;
        }
    }
    write_chops();
}

// Every chop not yet in its WAV file written there, on a thread of its own (a file is copied whole). A file that
// cannot be written (not a WAV, read-only) keeps its chop while the project is open.
void Context::write_chops() {
    std::vector<std::pair<std::string, SampleChop>> due;
    for (unsigned n = 0; n < sample_slots; ++n) {
        const auto& c = project.chops[n];
        const auto& ref = project.samples[n];
        if (!c.unwritten || !ref.used() || ref.instrument() || ref.path[0] != '/') continue;
        project.chops.edit(n).unwritten = false;
        due.emplace_back(ref.path, c);
    }
    if (due.empty()) return;
    std::thread([due = std::move(due)] { for (const auto& [path, chop] : due) write_mpc_slices(path, chop); }).detach();
}

// The sample's chop, as the MPC shows a sample's slices: whatever region the pad plays, Chop opens the slices the
// sample has (the region they span, the pad's slice selected). False: it has none.
bool Context::restore_chop() {
    if (ui.chop_sample >= sample_slots) return false;
    const auto& kept = project.chops[ui.chop_sample];
    if (!kept.used()) return false;
    const uint32_t frames = project.samples[ui.chop_sample].frames, pad_from = ui.chop_from;
    ui.chop_from = kept.from; ui.chop_to = kept.to;
    ui.slices = kept.slices;
    ui.chop_hand = kept.hand;
    if (ui.chop_hand.empty() && kept.mode == ChopMode::Manual) pin_manual_cuts(ui.chop_hand, ui.slices); // from the WAV: all by hand
    for (unsigned n = 0; n < ui.slices.count; ++n) // the slice the pad plays, selected
        if (ui.slices.start[n] <= pad_from && pad_from < ui.slices.end(n, frames ? frames : UINT32_MAX)) ui.chop_slice = uint8_t(n);
    ui.chop_mode = kept.mode;
    ui.chop_amount[size_t(kept.mode) % ui.chop_amount.size()] = kept.amount;
    ui.chop_take_at = 0; ui.chop_more = false;
    chop_unfollow(ui);
    return true;
}

uint32_t Context::zero_cut(uint32_t frame) const {
    if (!ui.settings.zero_snap || !library || ui.chop_sample >= sample_slots) return frame;
    return library->zero_crossing(ui.chop_sample, frame);
}

// Each cut inside the sample onto a zero crossing; the first slice's start
// and the last one's end too, unless they are the sample's own ends (its
// file starts and stops there already). A cut kept from its neighbours by
// move_start's 20 ms stays a little off the crossing.
void Context::snap_slices() {
    auto& sl = ui.slices;
    uint32_t frames = ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].frames : 0;
    if (!ui.settings.zero_snap || !frames || !sl.count) return;
    for (unsigned n = 0; n < sl.count; ++n)
        if (sl.start[n]) sl.move_start(n, zero_cut(sl.start[n]), frames);
    uint32_t last = sl.end(sl.count - 1, frames);
    if (last < frames) sl.move_end(sl.count - 1, zero_cut(last), frames);
}

} // namespace melodee
