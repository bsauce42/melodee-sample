// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/audio_player.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/audio_track.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Freeze plan F3, audio tracks (model/audio_track.h) on a thread of their own:
// each audio track's regions made into a pack the audio thread plays (each
// take read in place from its WAV file, the start of each region pinned), and
// a take drained from the engine host (Command::AudioRecord) into its file as
// it is recorded: "Take NNN.wav.part" while it grows, renamed once whole. The
// card's waits are this thread's, never the MIDI thread's or the audio thread's.
namespace melodee {

class EngineHost;
class StreamPager;

// A track's regions to make into a pack: those on its lane, the folder their takes are in, a bar's ticks.
struct AudioPackJob {
    unsigned track = 0;
    uint32_t serial = 0;
    std::string folder;
    unsigned bar_ticks = 384;
    std::vector<AudioRegion> regions;
};
struct AudioPackMade {
    unsigned track = 0;
    uint32_t serial = 0;
    AudioPack* pack = nullptr; // the MIDI thread's to send or discard
    unsigned missing = 0;      // regions whose take could not be read
    TakePeaks peaks;           // its takes' overviews (read beside them, or made once from the take)
};

// A take to record: its file, the track and the bars it is for, the tempo it is recorded at.
struct AudioTakeJob {
    std::string folder;
    unsigned take = 0, track = 0, from_bar = 0, to_bar = 0;
    uint16_t tempo = 1200;
};
struct AudioTakeDone {
    AudioTakeJob job;
    uint32_t frames = 0; // the file's (device) frames, at `rate`
    unsigned rate = 44100;
    std::string error;   // empty: the file is whole
    std::vector<uint8_t> peaks; // its overview (TakePeaks)
};

class AudioTracks {
public:
    explicit AudioTracks(EngineHost& host);
    ~AudioTracks();
    AudioTracks(const AudioTracks&) = delete;
    AudioTracks& operator=(const AudioTracks&) = delete;
    void set_pager(StreamPager* p) { pager = p; }

    // MIDI thread. A later pack job for the same track replaces one not started yet.
    void build(AudioPackJob job);
    bool take_made(AudioPackMade& out);
    void discard(AudioPack* pack); // freed here once the pager has let go of its streams
    // A take's file opened and drained from the engine host's audio take, until it is done; false when one is
    // still being written.
    bool record(const AudioTakeJob& job);
    bool take_recorded(AudioTakeDone& out);
    bool writing() const { return busy.load(std::memory_order_relaxed); }
    // The take number after every take in `folder` (files left by takes undone count too, so none is written twice).
    static unsigned next_take(const std::string& folder, unsigned at_least);
    // Files in `folder` no region of `kept` plays (takes undone, recorded over), removed; how many.
    static unsigned prune(const std::string& folder, const std::vector<unsigned>& kept);

private:
    void run();
    void make(const AudioPackJob& job);
    void drain();
    EngineHost& host;
    StreamPager* pager = nullptr;
    std::thread thread;
    mutable std::mutex lock;
    std::condition_variable wake;
    bool stopping = false;
    std::deque<AudioPackJob> jobs;
    std::deque<AudioPackMade> made;
    std::vector<AudioPack*> trash;
    // The take being written (this thread's once `taking` is set).
    bool taking = false, take_wanted = false;
    AudioTakeJob take_job;
    std::string take_part;
    WavStream take_file;
    unsigned take_rate = 44100;
    std::vector<float> chunk;
    std::vector<uint8_t> take_peaks;
    uint64_t take_peak_at = 0; // device frames taken into take_peaks
    std::deque<AudioTakeDone> recorded;
    static std::vector<uint8_t> peaks_of(const std::string& folder, unsigned take); // read beside it, else made and kept
    std::atomic<bool> busy {false};
};

} // namespace melodee
