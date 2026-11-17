// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Chop's pads (parity phase 5): a pad plays its slice and picks it for K1 /
// K2. In Manual, armed (K3 onto Manual, or Push), pad 1 plays the whole
// region and every pad tapped while it plays cuts it there (up to 16
// slices, as the MPC's), each cut pulled onto the strongest hit just before
// it, since a tap comes a little after the sound it means. With Zero Snap on
// (B2, kept with the settings) every cut then lands on a zero crossing.
#include "melodee/shell/context.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/mpc_wav_slices.h"
#include "melodee/model/slice_banks.h"
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
    if (ui.chop_take_at) { // Manual's take: a cut where the region is now
        uint32_t at = sl.start[0] + uint32_t(uint64_t(now - ui.chop_take_at) * 441 / 10);
        if (at < sl.end(sl.count - 1, frames)) {
            if (library) {
                auto loaded = library->slot(ui.chop_sample);
                at = zero_cut(snap_to_hit(at, loaded.hits, loaded.hit_count));
            }
            if (sl.add(at, frames, manual_slices)) {
                ui.chop_hand.pin(at); // a cut by hand: it stays when the type changes
                for (unsigned n = 0; n < sl.count; ++n) if (sl.start[n] == at) ui.chop_slice = uint8_t(n);
            }
            return;
        }
        ui.chop_take_at = 0; // it has played out: the pads play slices again
    }
    if (ui.chop_mode == ChopMode::Manual && ui.chop_armed && pad == 0) {
        if (sl.count) sl.whole(sl.start[0], sl.stop[sl.count - 1]); // the take starts from one slice, the whole region
        ui.chop_hand.clear();                                          // and its cuts are the hand cuts from here
        ui.chop_slice = 0; ui.chop_armed = false;
        ui.chop_take_at = now ? now : 1;
        audition(sl.start[0], sl.end(0, frames));
        return;
    }
    const unsigned slice = pad_slice(ui.chop_slice, pad); // the pad's slice in the bank in view (Pad Bank)
    if (slice >= sl.count) return;
    if (slice != ui.chop_slice) ui.chop_fine = false; // another slice: the wheel picks slices again until K1 / K2
    ui.chop_slice = uint8_t(slice);
    audition(sl.start[slice], sl.end(slice, frames));
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
    for (unsigned n = 0; n < ui.slices.count; ++n) // the slice the pad plays, selected
        if (ui.slices.start[n] <= pad_from && pad_from < ui.slices.end(n, frames ? frames : UINT32_MAX)) ui.chop_slice = uint8_t(n);
    ui.chop_mode = kept.mode;
    ui.chop_amount[size_t(kept.mode) % ui.chop_amount.size()] = kept.amount;
    ui.chop_armed = false;
    ui.chop_take_at = 0;
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

void Context::toggle_zero_snap() {
    ui.settings.zero_snap = !ui.settings.zero_snap;
    if (store) store->save_settings(ui.settings.text());
    if (ui.settings.zero_snap) snap_slices(); // what is there now joins in; off leaves the cuts where they are
    notice(ui.settings.zero_snap ? "Zero snap on" : "Zero snap off");
}

} // namespace melodee
