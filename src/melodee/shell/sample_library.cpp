// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/sample_library.h"
#include "melodee/model/card_folders.h"
#include "melodee/shell/sf2_file.h"
#include "melodee/shell/chop.h"
#include "melodee/shell/mpc_program.h"
#include "melodee/shell/sample_decode.h"
#include "melodee/shell/sample_memory.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "runtime/app.h"
#include "melodee/engine/engine_host.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <fstream>

namespace melodee {
namespace fs = std::filesystem;

struct SampleLibrary::Hearing {
    AudioReader file;
    SampleFill fill;
    std::vector<float> left, right;
    std::string path;
};

SampleLibrary::SampleLibrary(EngineHost& h) : host(h) {}
SampleLibrary::~SampleLibrary() = default;

bool SampleLibrary::load(unsigned slot, const std::string& path) {
    Request r;
    r.slot = uint16_t(slot % sample_slots);
    std::strncpy(r.path, path.c_str(), sizeof(r.path) - 1);
    if (!requests.push(r)) return false;
    slot_state[r.slot].store(0, std::memory_order_relaxed); // what it held is no longer how its load stands
    slot_held[r.slot].store(!path.empty(), std::memory_order_relaxed);
    asked.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void SampleLibrary::view(View& out) const { std::lock_guard<std::mutex> guard(lock); out = known; }
const LoadedSample& SampleLibrary::shown_at(unsigned slot) const {
    static const LoadedSample none;
    const auto& shown = slot_shown[slot % sample_slots];
    return shown ? shown->sample : none;
}
SampleLibrary::Shown& SampleLibrary::show(unsigned slot) {
    auto& shown = slot_shown[slot % sample_slots];
    if (!shown) shown = std::make_unique<Shown>();
    return *shown;
}
LoadedSample SampleLibrary::slot(unsigned n) const { std::lock_guard<std::mutex> guard(lock); return shown_at(n); }
SampleLibrary::Measure SampleLibrary::measure(unsigned n) const {
    std::lock_guard<std::mutex> guard(lock);
    const auto& s = shown_at(n);
    return {s.frames, s.shapes};
}
int SampleLibrary::zone_at(unsigned n, int key, int velocity) const {
    std::lock_guard<std::mutex> guard(lock);
    const auto& s = shown_at(n);
    return s.instrument ? melodee::zone_at(s.shapes, key, velocity) : -1;
}
std::string SampleLibrary::zone_file(unsigned n, unsigned zone) const {
    std::lock_guard<std::mutex> guard(lock);
    const auto& shown = slot_shown[n % sample_slots];
    if (!shown) return {};
    const auto& s = shown->sample;
    const auto& names = shown->parts;
    if (!s.instrument || zone >= s.shapes.size() || s.shapes[zone].part >= names.size()) return {};
    return names[s.shapes[zone].part];
}
bool SampleLibrary::thumbnail(unsigned n, uint32_t from, uint32_t to, uint8_t* out, unsigned columns) const {
    std::lock_guard<std::mutex> guard(lock);
    const auto& s = shown_at(n);
    if (!s.ready || !s.frames || !columns) return false;
    to = std::min(to ? to : s.frames, s.frames);
    from = std::min(from, to);
    double width = s.overview.size();
    double a = from * width / s.frames, b = std::max(a + 1, to * width / s.frames);
    for (unsigned c = 0; c < columns; ++c) {
        unsigned c0 = unsigned(a + (b - a) * c / columns), c1 = std::max(c0 + 1, unsigned(a + (b - a) * (c + 1) / columns));
        uint8_t peak = 0;
        for (unsigned k = c0; k < c1 && k < s.overview.size(); ++k) peak = std::max(peak, s.overview[k]);
        out[c] = peak;
    }
    return true;
}
float SampleLibrary::peak(unsigned n, uint32_t from, uint32_t to) const {
    std::lock_guard<std::mutex> guard(lock);
    const SampleData* data = data_of[n % sample_slots];
    if (!data || !data->frames) return -1;
    to = to ? data->own(to) : data->frames; // its own frames between the two
    from = data->own(from);
    float top = 0;
    for (uint32_t f = std::min(from, to); f < to; ++f) top = std::max({top, std::fabs(data->left[f]), std::fabs(data->right[f])});
    return top;
}

uint32_t SampleLibrary::zero_crossing(unsigned n, uint32_t frame) const {
    std::lock_guard<std::mutex> guard(lock);
    const SampleData* data = data_of[n % sample_slots];
    return data && data->frames ? melodee::zero_crossing(*data, frame) : frame;
}

uint32_t SampleLibrary::zero_crossing_toward(unsigned n, uint32_t frame, int way) const {
    std::lock_guard<std::mutex> guard(lock);
    const SampleData* data = data_of[n % sample_slots];
    return data && data->frames ? melodee::zero_crossing_toward(*data, frame, way) : frame;
}

bool SampleLibrary::detail(unsigned n, uint32_t from, uint32_t to, uint8_t* out, unsigned columns, uint8_t* const* sides) const {
    std::lock_guard<std::mutex> guard(lock);
    const SampleData* data = data_of[n % sample_slots];
    if (!data || !data->frames || !columns) return false;
    uint32_t frames = std::min<uint32_t>(data->frames, uint32_t(data->left.size()));
    to = std::min(data->own(to), frames); // its own frames between the two
    from = std::min(data->own(from), to);
    if (to <= from) return false;
    for (unsigned c = 0; c < columns; ++c) {
        uint32_t a = from + uint32_t(uint64_t(to - from) * c / columns), b = from + uint32_t(uint64_t(to - from) * (c + 1) / columns);
        float l = 0, r = 0; // at most 128 frames a column looked at, so a wide view stays cheap
        uint32_t stride = std::max<uint32_t>(1, (b - a) / 128);
        for (uint32_t f = a; f < std::max(b, a + 1) && f < to; f += stride) {
            l = std::max(l, std::fabs(data->left[f]));
            r = std::max(r, std::fabs(data->right[f]));
        }
        auto byte = [](float v) { return uint8_t(std::lround(std::min(1.0f, v) * 255)); };
        out[c] = byte(std::max(l, r));
        if (sides) { sides[0][c] = byte(l); sides[1][c] = byte(r); }
    }
    return true;
}

std::string SampleLibrary::root() const { std::lock_guard<std::mutex> guard(lock); return known.root; }
std::string SampleLibrary::folder() const { std::lock_guard<std::mutex> guard(lock); return known.folder; }
Drive SampleLibrary::drive() const { std::lock_guard<std::mutex> guard(lock); return known.drive; }
bool SampleLibrary::top() const { std::lock_guard<std::mutex> guard(lock); return known.top; }
int SampleLibrary::came_from() const { std::lock_guard<std::mutex> guard(lock); return known.came_from; }
std::vector<SampleFile> SampleLibrary::files() const { std::lock_guard<std::mutex> guard(lock); return known.files; }
std::string SampleLibrary::previewed() const { std::lock_guard<std::mutex> guard(lock); return preview_heard; }

void SampleLibrary::browse(Drive drive, const std::string& folder) {
    std::lock_guard<std::mutex> guard(lock);
    wanted.pending = true; wanted.up = false; wanted.drive = drive; wanted.folder = folder;
}
void SampleLibrary::remove(const std::string& path) {
    std::lock_guard<std::mutex> guard(lock);
    removals_wanted.push_back(path);
}

// One deletion a pass: the file or the folder, with all in it; an MPC 3 track's, program's or project's own data
// folder with it. The folder in view is then listed again.
void SampleLibrary::remove_service() {
    std::string path;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (removals_wanted.empty()) return;
        path = removals_wanted.front();
        removals_wanted.erase(removals_wanted.begin());
    }
    const fs::path file(path);
    std::error_code error;
    std::string kind = file.extension().string();
    for (auto& c : kind) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (!fs::is_directory(file, error))
        for (const auto& [suffix, tail] : {std::pair<const char*, const char*> {".XTY", "_[TrackData]"}, {".XPM", "_[ProgramData]"}, {".XPJ", "_[ProjectData]"}}) {
            if (kind != suffix) continue;
            const fs::path data = file.parent_path() / (file.stem().string() + tail);
            if (fs::is_directory(data, error)) fs::remove_all(data, error);
        }
    error.clear();
    fs::remove_all(file, error);
    std::error_code still;
    const bool gone = !error && !fs::exists(file, still);
    finish((gone ? "Deleted " : "Cannot delete ") + file.filename().string());
    removal_count.fetch_add(1, std::memory_order_release);
    std::lock_guard<std::mutex> guard(lock);
    if (!wanted.pending) { wanted.pending = true; wanted.up = false; wanted.drive = known.drive; wanted.folder = known.folder; }
}

void SampleLibrary::install(const std::string& path) {
    std::lock_guard<std::mutex> guard(lock);
    installs_wanted.push_back(path);
}

void SampleLibrary::install_service() {
    if (!installing) {
        std::string path, root;
        {
            std::lock_guard<std::mutex> guard(lock);
            if (installs_wanted.empty()) return;
            path = installs_wanted.front();
            installs_wanted.erase(installs_wanted.begin());
            root = known.root;
        }
        if (root.empty()) { finish("Install: no card"); return; }
        auto next = std::make_unique<ExpansionInstall>();
        auto error = next->open(path, (fs::path(root) / "Expansions").string());
        if (!error.empty()) { finish("Install: " + error); return; }
        finish("Installing " + next->title());
        installing = std::move(next);
        return;
    }
    std::string error;
    if (installing->step(error)) return;
    if (!error.empty()) finish("Install: " + error);
    else {
        finish("Installed " + installing->title());
        browse(Drive::Card, installing->folder()); // its programs, in view
    }
    installing.reset();
}

void SampleLibrary::browse_up() {
    std::lock_guard<std::mutex> guard(lock);
    wanted.pending = true; wanted.up = true;
}
void SampleLibrary::preview(const std::string& path) {
    std::lock_guard<std::mutex> guard(lock);
    preview_wanted = path; preview_pending = true;
    if (path.empty()) preview_heard.clear();
}

std::vector<std::string> SampleLibrary::internal_tops() {
    std::vector<std::string> tops;
    if (const char* forced = std::getenv("MELODEE_INTERNAL")) { // a host: folders split by ':'
        std::string list = forced;
        for (size_t at = 0; at <= list.size();) {
            size_t colon = list.find(':', at);
            if (colon == std::string::npos) colon = list.size();
            if (colon > at) tops.push_back(list.substr(at, colon - at));
            at = colon + 1;
        }
        return tops;
    }
    return {"/content/Samples", "/data/UserContent/Samples"}; // the factory library, the stock app's own
}

namespace {
std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return text;
}
bool is_sample_file(const fs::path& path) { // what a pad plays: WAV, MP3, FLAC, Ogg or AIFF (shell/sample_decode.cpp)
    const auto kind = upper(path.extension().string());
    return kind == ".WAV" || kind == ".MP3" || kind == ".FLAC" || kind == ".OGG" || kind == ".OGA" || kind == ".AIF" || kind == ".AIFF";
}
bool is_bank(const fs::path& path) { return upper(path.extension().string()) == ".MLDB"; }
bool is_instrument(const fs::path& path) { // an .sfz, an MPC keygroup program (.xpm, .xty), an EXS24 instrument, or a pad of an MPC program or project
    std::string file, which;
    if (split_program_pad(path.string(), file, which)) return true;
    unsigned preset = 0;
    if (split_sf2_preset(path.string(), file, preset)) return true; // roadmap P3: a preset of an .sf2
    const auto kind = upper(path.extension().string());
    return kind == ".SFZ" || kind == ".XPM" || kind == ".XTY" || kind == ".EXS" || kind == ".DSPRESET";
}
// Roadmap P3: an .sf2's presets as the browser lists a folder's files, each an instrument "<file>#<its place>",
// by bank and program, those with nothing to play left out.
void list_presets(const std::string& file, std::vector<SampleFile>& out) {
    auto mapped = open_sf2(file);
    if (!mapped->error.empty()) return;
    for (const auto& p : mapped->sf2.presets()) {
        if (!mapped->sf2.plays(p) || out.size() >= 4000) continue;
        SampleFile f;
        f.path = file + "#" + std::to_string(p.index);
        char number[16];
        std::snprintf(number, sizeof(number), p.bank ? "%d:%03d " : "%03d ", p.bank ? p.bank : p.program, p.program);
        f.name = upper(p.bank ? number + p.name : std::string(number, 4) + p.name).substr(0, 26);
        f.instrument = true;
        out.push_back(std::move(f));
    }
}
// "<name>_[TrackData]" beside <name>.xty (and an .xpj's, an .xpm's data folder beside its file).
bool owned_data(const fs::path& dir) {
    const std::string name = dir.filename().string();
    for (const char* tail : {"_[TrackData]", "_[ProjectData]", "_[ProgramData]"}) {
        const std::string t = tail;
        if (name.size() <= t.size() || name.compare(name.size() - t.size(), t.size(), t)) continue;
        const std::string stem = name.substr(0, name.size() - t.size());
        std::error_code error;
        for (const char* kind : {".xty", ".xpj", ".xpm", ".XTY", ".XPJ", ".XPM"})
            if (fs::exists(dir.parent_path() / (stem + kind), error)) return true;
    }
    return false;
}
bool is_expansion(const fs::path& path) { return upper(path.extension().string()) == ".XPN"; } // an MPC expansion: a zip to install
bool is_midi_file(const fs::path& path) { auto e = upper(path.extension().string()); return e == ".MID" || e == ".MIDI" || e == ".SMF"; } // roadmap P4
// A folder's folders, sample files and bank files, appended; hidden ones and lost+found left out.
// card_top: the card's own top folder, where its boot folder (autorun, what starts the app) is no sample's.
void list_into(const std::string& folder, std::vector<SampleFile>& out, bool card_top = false) {
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) return;
    for (auto it = fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, error);
         !error && it != fs::directory_iterator() && out.size() < 4000; it.increment(error)) {
        auto name = it->path().filename().string();
        if (name.empty() || name[0] == '.' || name == "lost+found" || (card_top && name == "autorun")) continue;
        bool folder_here = it->is_directory(error);
        if (folder_here && melodee_own_folder(it->path())) continue; // Melodee/Cache, Melodee/Crashes
        if (folder_here && name == "[Previews]") continue; // an expansion's program previews: Preview plays them on the program
        if (folder_here && owned_data(it->path())) continue;  // a program's or project's own samples: it is the row
        bool bank = !folder_here && is_bank(it->path());
        bool instrument = !folder_here && is_instrument(it->path());
        bool expansion = !folder_here && is_expansion(it->path());
        bool presets = !folder_here && is_sf2(it->path().string()); // roadmap P3: an .sf2 opens as a folder of its presets
        bool midi = !folder_here && is_midi_file(it->path());
        if (!folder_here && !(it->is_regular_file(error) && (is_sample_file(it->path()) || bank || instrument || expansion || presets || midi))) continue;
        SampleFile f;
        f.path = it->path().string(); f.folder = folder_here || presets; f.bank = bank; f.instrument = instrument; f.expansion = expansion;
        f.midi = midi;
        f.presets = presets;
        if (instrument && is_drum_program(f.path)) { f.kit = true; f.instrument = false; }
        f.name = upper(folder_here ? name : it->path().stem().string()).substr(0, 26);
        if (!folder_here) f.bytes = it->file_size(error);
        out.push_back(std::move(f));
    }
}
} // namespace

void SampleLibrary::service(Hardware& hardware) {
    const Platform& platform = hardware.platform;
    if (host.recorder().state() == TakeRecorder::State::Done) save_take();
    drain_render(); // a render's frames, as they come, every pass
    RecallJob recall_job;
    if (recalls.pop(recall_job)) save_recall(recall_job);
    stems_service(platform); // a sample's stems asked for, and the job that ended (shell/sample_library_stems.cpp)
    loop_service(); // the Looper's commit and files (shell/looper_files.cpp)
    host.streams().start();
    reap(); // samples the audio thread has let go of
    // Loads: what the pool's threads have read is handed on, and every file asked for since is begun there.
    take_loaded();
    Request request;
    while (requests.pop(request)) dispatch(request);
    landings_open.store(unsigned(landings.size()), std::memory_order_relaxed);
    log_load();
    SampleJob job;
    bank_files();
    install_service();
    remove_service();
    if (jobs.pop(job)) make(job);
    else {
        std::string heard;
        bool pending = false;
        { std::lock_guard<std::mutex> guard(lock); std::swap(pending, preview_pending); heard = preview_wanted; }
        if (pending && !heard.empty()) hear(heard);
    }
    hear_more(); // the preview's next piece, every pass, so the reading keeps ahead of what plays
    // The card's Samples folder: $MELODEE_SAMPLES, else the first card's, looked for every few seconds. A card
    // without one is still a card: what Melodee writes makes the folder, and browsing starts at the card's top.
    bool browse_wanted;
    { std::lock_guard<std::mutex> guard(lock); browse_wanted = wanted.pending; }
    if (++ticks % 600 == 1 || scan_wanted.load() || browse_wanted) {
        std::string root, top;
        if (const char* forced = std::getenv("MELODEE_SAMPLES")) root = top = forced;
        else
            for (const auto& space : platform.storage())
                if (space.path.rfind("/media/", 0) == 0) { top = space.path; root = space.path + "/Samples"; break; }
        bool changed = root != root_seen;
        if (changed) { root_seen = root; std::lock_guard<std::mutex> guard(lock); card_top = top; known.root = root; }
        if (changed) {
            cache.set_folder(top.empty() ? std::string() : top + "/Melodee/Cache");
            if (!top.empty() && !cache_tidied) { cache_tidied = true; pool.post([tidied = cache] { tidied.tidy(); }); } // a stat a file: not on this thread
        }
        scan_wanted.store(false);
        scan();
    }
}

// The folder the browser is in, after what the MIDI thread asked: another
// folder, up one, or another drive. Only a listing that differs counts as a change.
void SampleLibrary::scan() {
    Wanted ask;
    Drive drive;
    std::string folder;
    {
        std::lock_guard<std::mutex> guard(lock);
        ask = wanted; wanted.pending = false;
        drive = known.drive; folder = known.folder;
    }
    auto tops = internal_tops();
    std::error_code error;
    auto start_of = [&](Drive d) { return d == Drive::Internal ? std::string() : fs::is_directory(root_seen, error) ? root_seen : card_top; };
    std::string left; // going up: the folder left, to put the cursor on
    if (ask.pending && ask.up) {
        bool at_top = drive == Drive::Internal ? folder.empty() : (folder.empty() || folder == card_top);
        if (!at_top) {
            left = folder;
            auto parent = fs::path(folder).parent_path().string();
            bool to_top = drive == Drive::Internal && std::find(tops.begin(), tops.end(), parent) != tops.end();
            folder = to_top ? std::string() : parent;
        }
    } else if (ask.pending) {
        drive = ask.drive;
        folder = ask.folder.empty() ? start_of(drive) : ask.folder;
    }
    // A card taken out, or a folder gone: back to the drive's start.
    if (drive == Drive::Card && (card_top.empty() || folder.rfind(card_top, 0) != 0)) folder = start_of(drive);
    if (!folder.empty() && !fs::is_directory(folder, error) && !(is_sf2(folder) && fs::is_regular_file(folder, error))) folder = start_of(drive);
    std::vector<SampleFile> files;
    if (drive == Drive::Internal && folder.empty()) for (const auto& t : tops) list_into(t, files);
    else if (is_sf2(folder)) list_presets(folder, files); // inside an .sf2: its presets
    else list_into(folder, files, drive == Drive::Card && folder == card_top);
    // MPC drum programs (kits) first, then instruments (keygroup programs, SFZ), as an expansion's folder holds
    // them among its samples; then folders, then files.
    std::sort(files.begin(), files.end(), [](const SampleFile& a, const SampleFile& b) {
        if (a.kit != b.kit) return a.kit;
        if (a.instrument != b.instrument) return a.instrument;
        return a.folder != b.folder ? a.folder : a.name != b.name ? a.name < b.name : a.path < b.path;
    });
    int came_from = -1;
    for (size_t n = 0; n < files.size() && !left.empty(); ++n) if (files[n].path == left) came_from = int(n);
    bool top = drive == Drive::Internal ? folder.empty() : (folder.empty() || folder == card_top);
    std::lock_guard<std::mutex> guard(lock);
    bool same = known.drive == drive && known.folder == folder && known.files.size() == files.size()
        && std::equal(files.begin(), files.end(), known.files.begin(), [](const SampleFile& a, const SampleFile& b) { return a.path == b.path; });
    if (same && !ask.pending) return;
    known.drive = drive; known.folder = folder; known.top = top;
    known.files = std::move(files);
    known.came_from = came_from;
    changes.fetch_add(1, std::memory_order_release);
}

void SampleLibrary::finish(const std::string& text, int failed) {
    Done d;
    std::snprintf(d.text, sizeof(d.text), "%s", text.c_str());
    d.failed = int16_t(failed);
    finished.push(d);
}

// A name free in `folder`: the name, else "name 2", "name 3" ... (as Projects do).
std::string SampleLibrary::free_name(const std::string& folder, const std::string& name, const char* extension) const {
    std::string clean;
    for (char ch : name) clean += (ch == '/' || ch == '\\' || ch == ':' || ch == '"' || ch == '*' || ch == '?' || ch == '<' || ch == '>' || ch == '|') ? '-' : ch;
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    if (clean.empty()) clean = "Sample";
    std::error_code error;
    for (unsigned n = 1; n < 1000; ++n) {
        auto path = folder + "/" + clean + (n > 1 ? " " + std::to_string(n) : std::string()) + extension;
        if (!fs::exists(path, error)) return path;
    }
    return {};
}

void SampleLibrary::write_bank(const std::string& name, std::vector<uint8_t> bytes) {
    std::lock_guard<std::mutex> guard(lock);
    bank_writes.emplace_back(name, std::move(bytes));
}
void SampleLibrary::read_bank(const std::string& path) {
    std::lock_guard<std::mutex> guard(lock);
    bank_reads.push_back(path);
}
bool SampleLibrary::take_bank(BankBytes& out) {
    std::lock_guard<std::mutex> guard(lock);
    if (banks_read.empty()) return false;
    out = std::move(banks_read.front());
    banks_read.erase(banks_read.begin());
    return true;
}

// Kit bank files: a few kilobytes each, written and read whole. They go
// in the card's Samples/Banks, so the browser lists them with the samples.
void SampleLibrary::bank_files() {
    decltype(bank_writes) writes;
    decltype(bank_reads) reads;
    std::string root;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (bank_writes.empty() && bank_reads.empty()) return;
        std::swap(writes, bank_writes); std::swap(reads, bank_reads);
        root = known.root;
    }
    for (auto& [name, bytes] : writes) {
        if (root.empty()) { finish("Save Bank: no card"); continue; }
        std::error_code error;
        auto folder = root + "/Banks";
        fs::create_directories(folder, error);
        auto path = free_name(folder, name, ".mldb");
        if (path.empty()) { finish("Save Bank: no free name"); continue; }
        auto temporary = path + ".new";
        bool wrote;
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            wrote = bool(file);
        }
        if (wrote) fs::rename(temporary, path, error);
        if (!wrote || error) { fs::remove(temporary, error); finish("Save Bank: could not write"); continue; }
        finish("Saved " + fs::path(path).stem().string());
        scan_wanted.store(true); // the Banks folder, if the browser is in it
    }
    for (auto& path : reads) {
        BankBytes got;
        got.path = path;
        std::ifstream file(path, std::ios::binary);
        if (!file) got.error = "could not read";
        else got.bytes.assign(std::istreambuf_iterator<char>(file), {});
        std::lock_guard<std::mutex> guard(lock);
        banks_read.push_back(std::move(got));
    }
}

// Save Sample, Trim Sample, Extract: a slot's region as a WAV file on the
// card. Derive also puts the new file in a slot of its own (in memory when
// there is no card), as a load would.
// The region is cut from the source's frames and written at its rate, so
// nothing is converted.
// Extract's slice: 2 ms fades at the ends that were cut inside the source,
// as the sampler plays a region's (engine/sampler.cpp), so the file does not
// click where the slice was cut mid-wave.
static void fade_edges(SampleData& data, bool in, bool out) {
    uint32_t edge = std::min<uint32_t>(data.rate / 500, data.frames / 2);
    for (uint32_t k = 0; k < edge; ++k) {
        float g = float(k) / float(edge);
        if (in) { data.left[k] *= g; data.right[k] *= g; }
        if (out) { data.left[data.frames - 1 - k] *= g; data.right[data.frames - 1 - k] *= g; }
    }
}

void SampleLibrary::make(const SampleJob& job) {
    const char* what = job.kind == SampleJob::Kind::Save ? "Save Sample" : "Sample";
    const SampleData* source = job.source < sample_slots ? data_of[job.source] : nullptr; // only this thread writes it
    auto fail = [&](const std::string& why) { // the MIDI thread lets go of the slot it held
        finish(std::string(what) + ": " + why, job.kind == SampleJob::Kind::Derive ? int(job.target % sample_slots) : -1);
    };
    if (!source || !source->frames) { fail("not loaded"); return; }
    // The region (44.1 kHz frames, as the project counts them; `to` 0 the end) in the source's own frames.
    const uint32_t all = source->frames, end = job.to && job.to < source->model_frames() ? source->own(job.to) : all;
    const uint32_t first = std::min(source->own(job.from), end);
    if (end - first < 2) { fail("too short"); return; }
    auto data = std::make_unique<SampleData>();
    adopt(*data, std::vector<float>(source->left.begin() + first, source->left.begin() + end),
          std::vector<float>(source->right.begin() + first, source->right.begin() + end), source->rate);
    if (job.edges) fade_edges(*data, first > 0, end < all);
    as_written(*data); // what plays is what the file holds
    std::string root;
    { std::lock_guard<std::mutex> guard(lock); root = known.root; }
    std::string name(job.name), path, error;
    if (!root.empty()) {
        std::string folder = job.folder[0] ? root + "/" + job.folder : root;
        std::error_code made;
        fs::create_directories(folder, made);
        path = free_name(folder, name);
        error = path.empty() ? std::string("no free name") : write_wav(path, *data);
        if (!error.empty()) path.clear();
        else name = fs::path(path).stem().string();
    } else error = "no card";
    if (job.kind == SampleJob::Kind::Save) {
        if (path.empty()) { fail(error); return; }
        finish("Saved " + upper(name));
        scan_wanted.store(true); // it shows in the browser
        return;
    }
    LoadedSample loaded;
    loaded.name = upper(name).substr(0, 27);
    loaded.path = path;
    loaded.frames = data->model_frames();
    loaded.stereo = make_overview(*data, loaded.overview, &loaded.sides);
    loaded.hit_count = find_hits(*data, loaded.hits, loaded.beats);
    if (!offer(job.target % sample_slots, data.get())) { fail("busy, try again"); return; }
    const SampleData* held = data.get();
    owned.push_back(std::move(data));
    loaded.ready = true;
    if (!path.empty()) scan_wanted.store(true);
    else finish("Not on the card: " + error);
    std::lock_guard<std::mutex> guard(lock);
    show(job.target).sample = loaded;
    data_of[job.target % sample_slots] = held;
    changes.fetch_add(1, std::memory_order_release);
}

// The browser's preview (B3): the file in the preview slot, at most 30 s of it. It plays once its first piece is
// read, not when all of it is: the sample is laid out at its full length and filled in a piece a pass (hear_more()),
// and the voice reads only what has landed (SampleGrowth), so a long file starts as soon as a short one.
void SampleLibrary::hear(const std::string& path) {
    stop_hearing(); // the last one's file goes before anything takes its place in the preview slot
    // An MPC program with a preview of its own (an expansion's [Previews]): that, as the MPC plays it.
    const std::string preview = is_mpc_program(path) ? program_preview(path) : std::string();
    if (preview.empty() && is_expansion(path)) return;
    if (preview.empty() && is_drum_program(path)) return; // a kit with no preview: nothing to hear
    if (preview.empty() && is_instrument(path)) { hear_instrument(path); return; }
    auto h = std::make_unique<Hearing>();
    if (!h->file.open(preview.empty() ? path : preview, 30).empty()) return;
    auto data = std::make_unique<SampleData>();
    h->fill.begin(*data, h->file.frames(), h->file.rate());
    h->path = path;
    hearing = std::move(h);
    if (!hear_piece() && !data->growth->whole.load(std::memory_order_relaxed) && !data->growth->frames.load(std::memory_order_relaxed))
        return; // not a frame of it could be read
    if (!offer(preview_slot, data.get())) { stop_hearing(); return; }
    owned.push_back(std::move(data));
    { std::lock_guard<std::mutex> guard(lock); preview_heard = path; }
    preview_count.fetch_add(1, std::memory_order_release);
}

bool SampleLibrary::hear_piece() {
    if (!hearing) return false;
    auto& h = *hearing;
    const uint32_t piece = std::max(1u, h.file.rate() / 8); // an eighth of a second: a pass every few ms keeps ahead
    h.left.resize(piece); h.right.resize(piece);
    h.fill.add(h.left.data(), h.right.data(), h.file.read(h.left.data(), h.right.data(), piece));
    if (h.fill.to_come() && !h.file.ended()) return true;
    stop_hearing(); // all of it, or the file ended early: cut there
    return false;
}

void SampleLibrary::hear_more() {
    if (!hearing) return;
    bool wanted;
    { std::lock_guard<std::mutex> guard(lock); wanted = preview_wanted == hearing->path; }
    if (wanted) hear_piece();
    else stop_hearing();
}

void SampleLibrary::stop_hearing() {
    if (!hearing) return;
    hearing->fill.finish();
    hearing.reset();
}

// A finished take: copied out of the recorder, written to Samples/Recorded on
// the card when there is one, and handed back as the slot the take was armed for.
void SampleLibrary::save_take() {
    auto& recorder = host.recorder();
    auto data = std::make_unique<SampleData>();
    data->frames = recorder.taken(); // its own frames, at the rate it was taken at
    data->left.assign(recorder.left(), recorder.left() + data->frames);
    data->right.assign(recorder.right(), recorder.right() + data->frames);
    unsigned slot = recorder.slot() % sample_slots, rate = recorder.take_rate();
    recorder.release();
    land(slot, std::move(data), rate, "Recorded", "TAKE", "Take");
}

bool SampleLibrary::offer(unsigned slot, SampleData* data) {
    if (data && slot < sample_slots) slot_held[slot].store(true, std::memory_order_relaxed);
    return host.offer_sample(slot, data);
}

void SampleLibrary::land(unsigned slot, std::unique_ptr<SampleData> data, unsigned rate, const char* folder, const char* stem,
                         const char* what) {
    slot %= sample_slots;
    unsigned number = take_count.fetch_add(1) + 1;
    LoadedSample loaded;
    loaded.name = std::string(stem) + " " + std::to_string(number);
    std::string root, path;
    { std::lock_guard<std::mutex> guard(lock); root = known.root; }
    if (!root.empty()) {
        std::error_code error;
        std::string dir = root + "/" + folder;
        fs::create_directories(dir, error);
        // The first free number, so the ones from earlier sessions stay.
        for (unsigned n = 1; n < 10000 && path.empty(); ++n) {
            auto name = dir + "/" + stem + " " + std::to_string(n) + ".wav";
            if (fs::exists(name, error)) continue;
            loaded.name = std::string(stem) + " " + std::to_string(n);
            path = name;
        }
    }
    // Kept and written at its own rate.
    auto failed = keep_capture(*data, rate, path);
    if (!path.empty()) { if (failed.empty()) loaded.path = path; else loaded.error = "not saved: " + failed; }
    loaded.frames = data->model_frames();
    loaded.stereo = make_overview(*data, loaded.overview, &loaded.sides);
    loaded.hit_count = find_hits(*data, loaded.hits, loaded.beats);
    const SampleData* held = nullptr;
    if (offer(slot, data.get())) { held = data.get(); owned.push_back(std::move(data)); loaded.ready = true; }
    else { finish(std::string(what) + ": busy, try again", int(slot)); return; }
    if (!loaded.path.empty()) scan_wanted.store(true); // it shows in the browser
    std::lock_guard<std::mutex> guard(lock);
    show(slot).sample = loaded;
    data_of[slot] = held;
    changes.fetch_add(1, std::memory_order_release);
}

namespace {
// A step timed into one of the load's sums.
struct Lap {
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
    void into(std::atomic<uint64_t>& sum) {
        const auto now = std::chrono::steady_clock::now();
        sum.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - at).count()), std::memory_order_relaxed);
        at = now;
    }
};
} // namespace

bool SampleLibrary::instrument_path(const std::string& path) { return is_instrument(path); }

SampleLibrary::SlotLoad SampleLibrary::slot_load(unsigned slot) const {
    const uint32_t packed = slot_state[slot % sample_slots].load(std::memory_order_relaxed);
    SlotLoad out;
    out.settled = (packed >> 31) != 0;
    out.instrument = ((packed >> 30) & 1) != 0;
    out.parts = (packed >> 15) & 0x7fff;
    out.parts_in = packed & 0x7fff;
    return out;
}

void SampleLibrary::set_slot_state(unsigned slot, bool settled, bool instrument, unsigned parts, unsigned parts_in) {
    slot_state[slot % sample_slots].store((settled ? 1u << 31 : 0) | (instrument ? 1u << 30 : 0) | (std::min(parts, 0x7fffu) << 15) | std::min(parts_in, 0x7fffu),
                                          std::memory_order_relaxed);
}

void SampleLibrary::dispatch(const Request& request) {
    const unsigned slot = request.slot % sample_slots;
    const uint32_t serial = ++slot_serial[slot]; // what an earlier load of this slot still brings is dropped
    cancel_landing(slot);
    set_slot_state(slot, false);
    if (!request.path[0]) { // an empty slot in the project now: nothing plays from it
        Handover h;
        h.slot = uint16_t(slot); h.serial = serial; h.clear = true;
        handovers.push_back(std::move(h));
        return;
    }
    if (!load_log.open) {
        load_log = LoadLog {};
        load_log.open = true;
        load_log.since = std::chrono::steady_clock::now();
        load_times.read = 0; load_times.analyse = 0; load_times.parse = 0; load_times.land = 0;
    }
    if (is_instrument(request.path)) {
        ++load_log.instruments;
        set_slot_state(slot, false, true);
        pool.post([this, request, serial] {
            now_loading.store(request.slot, std::memory_order_relaxed);
            InstrumentRead out;
            out.request = request; out.serial = serial;
            Lap lap;
            out.read = read_instrument(request.path, &out.error);
            lap.into(load_times.parse);
            std::lock_guard<std::mutex> guard(loaded_lock);
            instruments_read.push_back(std::move(out));
        });
        return;
    }
    ++load_log.files;
    pool.post([this, request, serial] {
        now_loading.store(request.slot, std::memory_order_relaxed);
        Handover h;
        h.slot = request.slot; h.serial = serial;
        h.data = std::make_unique<SampleData>();
        Lap lap;
        std::string error;
        { // what it will hold, found before it is read: refused where that would leave memory short (shell/sample_memory.h)
            AudioReader file;
            if (file.open(request.path, kit_sample_seconds).empty()
                && !memory_allows(memory_free(), kit_sample_bytes(file.frames(), file.rate()))) {
                error = "Memory full";
                h.refused = true;
            }
        }
        if (error.empty()) error = read_audio(request.path, *h.data, kit_sample_seconds, nullptr); // at the file's own rate
        lap.into(load_times.read);
        LoadedSample& loaded = h.loaded;
        loaded.path = request.path;
        loaded.name = upper(fs::path(request.path).stem().string());
        if (!error.empty()) {
            loaded.error = error;
        } else {
            loaded.frames = h.data->model_frames();
            loaded.stereo = make_overview(*h.data, loaded.overview, &loaded.sides);
            loaded.hit_count = find_hits(*h.data, loaded.hits, loaded.beats);
            lap.into(load_times.analyse);
        }
        std::lock_guard<std::mutex> guard(loaded_lock);
        files_loaded.push_back(std::move(h));
    });
}

// A sample read (or a slot emptied) to the audio thread and into what the pages see. False while that thread's
// queue has no room.
bool SampleLibrary::hand_over(Handover& h) {
    const unsigned slot = h.slot % sample_slots;
    if (h.serial != slot_serial[slot]) { answered.fetch_add(1, std::memory_order_release); return true; } // asked for again since
    const SampleData* held = nullptr;
    if (h.clear) {
        if (!host.offer_sample(slot, nullptr)) return false;
    } else if (h.loaded.error.empty()) {
        if (!offer(slot, h.data.get())) return false;
        held = h.data.get();
        owned.push_back(std::move(h.data));
        h.loaded.ready = true;
    } else {
        ++load_log.failed;
        if (h.refused && !load_log.refused++) finish("Memory full: sample not loaded"); // once for a project's or a kit's load
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        if (h.clear) slot_shown[slot].reset();
        else show(slot).sample = std::move(h.loaded);
        if (held || h.clear) data_of[slot] = held;
        changes.fetch_add(1, std::memory_order_release);
    }
    set_slot_state(slot, true);
    answered.fetch_add(1, std::memory_order_release);
    return true;
}

void SampleLibrary::take_loaded() {
    std::vector<Handover> files;
    std::vector<InstrumentRead> instruments;
    std::vector<PartLanded> parts;
    {
        std::lock_guard<std::mutex> guard(loaded_lock);
        files.swap(files_loaded); instruments.swap(instruments_read); parts.swap(parts_landed);
    }
    for (auto& f : files) handovers.push_back(std::move(f));
    for (auto& i : instruments) instruments_waiting.push_back(std::move(i));
    // In order, as the audio thread's queue has room (it takes them a block at a time). One refused for three
    // seconds (the audio thread is not running) is given up, as a load that found the queue full always was.
    // What the audio thread lets go of is taken back before each: a project of hundreds of samples switched to
    // replaces hundreds, and its queue of those let go holds 128.
    while (!handovers.empty()) {
        reap();
        if (hand_over(handovers.front())) { handovers.pop_front(); handover_refused = false; continue; }
        const auto now = std::chrono::steady_clock::now();
        if (!handover_refused) { handover_refused = true; handover_since = now; }
        if (now - handover_since < std::chrono::seconds(3)) break;
        Handover& h = handovers.front();
        if (!h.clear) {
            h.loaded.error = "busy, try again";
            ++load_log.failed;
            std::lock_guard<std::mutex> guard(lock);
            show(h.slot).sample = std::move(h.loaded);
            changes.fetch_add(1, std::memory_order_release);
        }
        set_slot_state(h.slot, true);
        answered.fetch_add(1, std::memory_order_release);
        handovers.pop_front();
        handover_refused = false;
    }
    size_t begun = 0;
    for (; begun < instruments_waiting.size(); ++begun) {
        reap(); // as above
        if (!begin_landing(instruments_waiting[begun])) break;
    }
    instruments_waiting.erase(instruments_waiting.begin(), instruments_waiting.begin() + std::ptrdiff_t(begun));
    for (auto& p : parts) {
        Landing& l = *p.landing;
        if (l.cancelled.load(std::memory_order_relaxed)) continue; // its slot holds something else now
        ++l.done;
        ++load_log.parts;
        if (p.laid_out) ++load_log.laid_out;
        if (!p.landed) ++load_log.failed;
        const unsigned slot = l.slot;
        unsigned in = 0;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto& shown = show(slot).sample;
            if (p.landed) ++shown.parts_ready;
            else { ++shown.parts_failed; if (shown.error.empty()) shown.error = p.why; }
            if (p.index < shown.part_state.size()) shown.part_state[p.index] = p.landed ? PartState::Landed : PartState::Failed;
            in = shown.parts_ready + shown.parts_failed;
            changes.fetch_add(1, std::memory_order_release);
        }
        const bool done = l.done >= l.multi->part_count;
        set_slot_state(slot, done, true, l.multi->part_count, in);
        if (done) landings.erase(std::remove(landings.begin(), landings.end(), p.landing), landings.end());
    }
}

void SampleLibrary::log_load() {
    if (!load_log.open || pool.pending() || !handovers.empty() || !instruments_waiting.empty() || !landings.empty()) return;
    {
        std::lock_guard<std::mutex> guard(loaded_lock);
        if (!files_loaded.empty() || !instruments_read.empty() || !parts_landed.empty()) return;
    }
    const auto& l = load_log;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - l.since).count();
    auto spent = [](const std::atomic<uint64_t>& us) { return (unsigned long long)(us.load(std::memory_order_relaxed) / 1000); };
    std::printf("load: %u samples, %u instruments (%u files, %u laid out), %u failed, %lld ms; threads: read %llu, analyse %llu, instruments %llu, their files %llu ms\n",
                l.files, l.instruments, l.parts, l.laid_out, l.failed, (long long)ms, spent(load_times.read), spent(load_times.analyse),
                spent(load_times.parse), spent(load_times.land));
    std::fflush(stdout);
    load_log.open = false;
    now_loading.store(-1, std::memory_order_relaxed);
}

} // namespace melodee
