# SPDX-License-Identifier: AGPL-3.0-only
"""A project written as an MPC 3 project by the built app, then read back."""
import gzip
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from test_mpc_project import BINARY, instrument, note, project, wav, write


def float_wav(path, rate, channels, frames):
    """A print as Melodee writes one: 32-bit float, each channel's sample its number."""
    data = b"".join(struct.pack("<f", float(c + 1)) for _ in range(frames) for c in range(channels))
    fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4, channels * 4, 32)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    Path(path).write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def read(path):
    lines = gzip.decompress(Path(path).read_bytes()).decode().split("\n", 5)
    return lines[:5], json.loads(lines[5])["data"]


@unittest.skipUnless(BINARY.is_file(), "build/melodee is not built")
class Mpc3Export(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True, timeout=120)

    def export(self, folder, *changes, pads=None):
        source = Path(folder) / "in"
        data = source / "Song_[ProjectData]"
        data.mkdir(parents=True)
        (data / "Kick.wav").write_bytes(wav(44100))
        (data / "Snare.wav").write_bytes(wav(22050))
        path = write(source, "Song", project(pads or [instrument("Kick.wav", 10, 90, group=2),
                                                      instrument("Snare.wav", 10, 40, volume=48 / 127, filter_type=3, cutoff=0.5, one_shot=False)]))
        out = Path(folder) / "out"
        out.mkdir()
        result = self.run_tool("--mpc3-export", path, out, "Beat", *[c.replace("FOLDER", str(folder)) for c in changes])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout, out

    def test_document(self):
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder)
            self.assertIn("exported tracks=1 keygroups=0 audio=0 regions=0 sequences=2 song=0 pads=2 notes=3 samples=2 skipped_tracks=0 skipped_pads=0 missing=0", report)
            header, data = read(out / "Beat.xpj")
            self.assertEqual(header[0], "ACVS")
            self.assertTrue(header[1].startswith("3."), header)
            self.assertEqual(header[2:4], ["SerialisableProjectData", "json"])
            self.assertEqual(data["version"], 30)
            self.assertEqual(sorted((out / "Beat_[ProjectData]").iterdir()), [out / "Beat_[ProjectData]" / "Kick.wav", out / "Beat_[ProjectData]" / "Snare.wav"])
            self.assertEqual(sorted(s["path"] for s in data["samples"]), ["Kick.wav", "Snare.wav"])

            kinds = [(t["name"], t["program"]["type"]) for t in data["tracks"]]
            self.assertEqual(kinds[0], ("Drum 001", 0))
            self.assertEqual(sum(1 for _, k in kinds if k == 9), 16)  # the Out pairs come with the template
            drum = data["tracks"][0]["program"]
            pads = drum["drum"]["instruments"]
            self.assertEqual(len(pads), 128)
            self.assertEqual(pads[0]["layersv"][0]["sampleFile"], "Kick.wav")
            self.assertEqual(pads[0]["whichMuteGroup"], 2)
            # 22.05 kHz: Melodee's 44.1 kHz frames 20..80 are the file's 10..40 again
            self.assertEqual((pads[1]["layersv"][0]["sliceInfo"]["Start"], pads[1]["layersv"][0]["sliceInfo"]["End"]), (10, 40))
            self.assertEqual(pads[1]["synthSection"]["filterData"]["value0"]["filterType"], 3)
            self.assertIsInstance(pads[1]["mixable"]["volume"], float)
            self.assertEqual((pads[0]["monophonic"], pads[0]["polyphony"]), (True, 1))

            # Assignments name only tracks the project has (the template's own are gone)
            names = {t["name"] for t in data["tracks"]}
            def targets(node):
                if isinstance(node, dict):
                    for key, value in node.items():
                        if key == "track" and isinstance(value, str):
                            yield value
                        else:
                            yield from targets(value)
                elif isinstance(node, list):
                    for value in node:
                        yield from targets(value)
            self.assertTrue(all(not t or t in names for t in targets(data)))

            self.assertEqual(len(data["sequences"]), 128)
            self.assertEqual(data["sequences"][0]["key"], 127)
            second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
            self.assertEqual(second["bpm"], 72.5)
            self.assertEqual(second["timeSignatureTrack"]["timeSignatures"][0]["beatsPerBar"], 3)
            clips = second["trackClipMaps"][0]
            self.assertEqual([c["key"] for c in clips], sorted(c["key"] for c in clips))
            events = next(c["value"] for c in clips if c["key"] == "Drum 001")["eventList"]["events"]
            self.assertEqual([(e["time"], e["note"]["note"]) for e in events], [(0, 36), (960, 37)])
            self.assertEqual(data["masterTempoEnabled"], False)
            self.assertEqual(len(data["clipPlayerData"]["trackClipTransportMap"]), len(clips))

    def test_missing_sample_left_out(self):
        # A pad whose file is not there is left out, not named: MPC 3 stops on a project naming a sample it cannot find.
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder, pads=[instrument("Kick.wav", 10, 90), instrument("Gone.wav", 0, 50)])
            self.assertIn("pads=1", report)
            self.assertIn("missing=1", report)
            _, data = read(out / "Beat.xpj")
        named = json.dumps(data)
        self.assertNotIn("Gone", named)
        self.assertEqual(data["tracks"][0]["program"]["drum"]["instruments"][1]["layersv"][0]["sampleFile"], "")

    def test_read_back(self):
        # What the export wrote reads back as what went in (shell/mpc_project.cpp reads MPC 3's projects too).
        with tempfile.TemporaryDirectory() as folder:
            _, out = self.export(folder)
            result = self.run_tool("--mpc-import", out / "Beat.xpj")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = result.stdout
        self.assertIn("name=Beat tracks=1 sequences=128 pads=2 notes=3 over_limit=0 missing=0 tempo=725 seq_tempo=1 signature=3/4", text)
        self.assertIn("pad A01 file=Kick start=10 end=90 level=100 pan=0 pitch=0 play=1 mono=1 group=2 filter=Off", text)
        self.assertIn("pad A02 file=Snare start=20 end=80 level=51 pan=0 pitch=0 play=0 mono=1 group=0 filter=LPF4 cutoff=50", text)
        self.assertIn("pattern 0 1 length=768 notes=2 first=0:36:127", text)
        self.assertIn("pattern 0 0 length=768 notes=1 first=0:37:127", text)

    def test_keygroup_instrument(self):
        # An SFZ of two key zones on the Snare's pad: a Keygroup track of its own, the pad's notes with it at
        # 60 + their pitch; the kit keeps the Kick.
        with tempfile.TemporaryDirectory() as folder:
            keys = Path(folder) / "Piano"
            keys.mkdir()
            (keys / "low.wav").write_bytes(wav(44100))
            (keys / "high.wav").write_bytes(wav(48000))
            (keys / "Piano.sfz").write_text("<region> sample=low.wav lokey=0 hikey=59 pitch_keycenter=48\n"
                                            "<region> sample=high.wav lokey=60 hikey=127 pitch_keycenter=72 tune=10 volume=-6\n")
            report, out = self.export(folder, "0.1=FOLDER/Piano/Piano.sfz", "0.1^5")
            self.assertIn("exported tracks=1 keygroups=1 audio=0 regions=0 sequences=2 song=0 pads=1 notes=3 samples=3", report)
            _, data = read(out / "Beat.xpj")
        tracks = {t["name"]: t for t in data["tracks"]}
        piano = tracks["Piano"]["program"]
        self.assertEqual((piano["type"], piano["keygroup"]["numKeygroups"]), (1, 2))
        groups = [i for i in piano["drum"]["instruments"] if i["layersv"][0]["sampleFile"]]
        self.assertEqual([(g["lowNote"], g["highNote"], g["layersv"][0]["rootNote"], g["layersv"][0]["sampleFile"]) for g in groups],
                         [(0, 59, 49, "low.wav"), (60, 127, 73, "high.wav")])
        self.assertEqual(groups[1]["layersv"][0]["fineTune"], 10)
        self.assertAlmostEqual(groups[1]["layersv"][0]["volume"]["gainCoefficient"], 10 ** (-6 / 20), places=4)
        self.assertEqual(tracks["Drum 001"]["program"]["drum"]["instruments"][1]["layersv"][0]["sampleFile"], "")
        second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
        clips = {c["key"]: c["value"]["eventList"]["events"] for c in second["trackClipMaps"][0]}
        self.assertEqual([(e["time"], e["note"]["note"]) for e in clips["Piano"]], [(960, 65)])
        self.assertEqual([(e["time"], e["note"]["note"]) for e in clips["Drum 001"]], [(0, 36)])

    def test_pitched_wav_pad(self):
        # A WAV pad its notes play 7 semitones up (16 Levels, Keys): one keygroup over every key, its own pitch at 60.
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder, "0.0^7")
            self.assertIn("exported tracks=1 keygroups=1", report)
            _, data = read(out / "Beat.xpj")
        tracks = {t["name"]: t for t in data["tracks"]}
        kick = tracks["Kick"]["program"]
        group = kick["drum"]["instruments"][0]
        self.assertEqual((group["lowNote"], group["highNote"], group["layersv"][0]["rootNote"], group["layersv"][0]["sampleFile"]), (0, 127, 61, "Kick.wav"))
        second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
        clips = {c["key"]: c["value"]["eventList"]["events"] for c in second["trackClipMaps"][0]}
        self.assertEqual([(e["time"], e["note"]["note"]) for e in clips["Kick"]], [(0, 67)])

    def test_layered_pad_stays_a_pad(self):
        # A pad of two velocity layers (an MPC pad's, read as an instrument) is still a pad of the kit.
        with tempfile.TemporaryDirectory() as folder:
            pad = instrument("Kick.wav", 10, 90)
            soft = dict(pad["layersv"][0], sampleFile="Snare.wav", velocityStart=0, velocityEnd=63)
            pad["layersv"][0] = dict(pad["layersv"][0], velocityStart=64, velocityEnd=127)
            pad["layersv"].append(soft)
            report, out = self.export(folder, pads=[pad])
            self.assertIn("exported tracks=1 keygroups=0", report)
            _, data = read(out / "Beat.xpj")
        layers = [l for l in data["tracks"][0]["program"]["drum"]["instruments"][0]["layersv"] if l["sampleFile"]]
        self.assertEqual(sorted(l["sampleFile"] for l in layers), ["Kick.wav", "Snare.wav"])

    def test_prints_as_audio_track(self):
        # A plug-in track's prints: an Audio track, each printed pattern a region from its sequence's start; a print
        # with pad sends' channels made stereo.
        with tempfile.TemporaryDirectory() as folder:
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A1.wav", 48000, 2, 1000)
            float_wav(prints / "T02 A2.wav", 48000, 4, 500)
            report, out = self.export(folder, f"1@0={prints}/T02 A1.wav", f"1@1={prints}/T02 A2.wav")
            self.assertIn("exported tracks=1 keygroups=0 audio=1 regions=2 sequences=2 song=0", report)
            _, data = read(out / "Beat.xpj")
            stereo = (out / "Beat_[ProjectData]" / "Synth A2.wav").read_bytes()
        tracks = {t["name"]: t for t in data["tracks"]}
        self.assertEqual(tracks["Synth"]["program"]["type"], 6)
        self.assertEqual(sorted(s["path"] for s in tracks["Synth"]["program"]["samples"]), ["Synth A1.wav", "Synth A2.wav"])
        channels, rate = struct.unpack("<HI", stereo[22:28])
        self.assertEqual((channels, rate), (2, 48000))
        self.assertEqual(struct.unpack("<ff", stereo[44:52]), (1.0, 2.0))
        self.assertEqual(len(stereo), 44 + 500 * 8)
        for key, file, frames in ((0, "Synth A1.wav", 1000), (1, "Synth A2.wav", 500)):
            sequence = next(s["value"] for s in data["sequences"] if s["key"] == key)
            events = next(c["value"] for c in sequence["trackClipMaps"][0] if c["key"] == "Synth")["eventList"]["events"]
            self.assertEqual([(e["type"], e["time"], e["audio"]["sample"]["path"], e["audio"]["sliceEnd"]) for e in events][0], (4, 0, file, frames))

    def test_print_loops_in_a_longer_sequence(self):
        # A one-bar print in a two-bar sequence (the kit's pattern is longer): the clip spans the sequence, the print
        # once each bar, as Melodee loops it.
        with tempfile.TemporaryDirectory() as folder:
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A1.wav", 44100, 2, 100)
            _, out = self.export(folder, f"1@0={prints}/T02 A1.wav")
            _, data = read(out / "Beat.xpj")
        sequence = next(s["value"] for s in data["sequences"] if s["key"] == 0)
        clip = next(c["value"] for c in sequence["trackClipMaps"][0] if c["key"] == "Synth")
        self.assertEqual(clip["endPulses"], sequence["lengthPulses"])
        times = [(e["time"], e["audio"]["length"]) for e in clip["eventList"]["events"]]
        self.assertEqual(times[0], (0, 3840))
        self.assertEqual(sum(length for _, length in times), sequence["lengthPulses"])

    def test_song(self):
        # The song as one sequence "Song" in the first free pattern number (2): pattern 1 (two notes in a 768-tick
        # pattern) at bar 0 for 4 bars of 3/4 plays twice and a little; a print's clip from bar 2; it plays first.
        with tempfile.TemporaryDirectory() as folder:
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A1.wav", 44100, 2, 100)
            report, out = self.export(folder, f"1@0={prints}/T02 A1.wav", "S:0.1@0+4", "S:1.0@2+2")
            self.assertIn("sequences=2 song=4 pads=", report)
            _, data = read(out / "Beat.xpj")
        song = next(s["value"] for s in data["sequences"] if s["key"] == 2)
        self.assertEqual((song["name"], song["lengthBars"], song["lengthPulses"]), ("Song", 4, 4 * 2880))
        self.assertEqual(data["currentSequence"], 2)
        clips = {c["key"]: c["value"] for c in song["trackClipMaps"][0]}
        drum = [(e["time"], e["note"]["note"]) for e in clips["Drum 001"]["eventList"]["events"]]
        self.assertEqual(drum, [(0, 36), (960, 37), (7680, 36), (8640, 37)])
        self.assertEqual(clips["Drum 001"]["endPulses"], 4 * 2880)
        audio = [(e["time"], e["audio"]["length"]) for e in clips["Synth"]["eventList"]["events"]]
        self.assertEqual(audio[0][0], 2 * 2880)
        self.assertEqual(sum(length for _, length in audio), 2 * 2880)

    def test_tracks_follow_the_sequence(self):
        # A one-bar pattern in a two-bar sequence plays twice, written out; every track as long as the sequence
        # (MPC 3's drum tracks were two bars of their own and looped them).
        with tempfile.TemporaryDirectory() as folder:
            # Drum 001's pattern 1 cut to 384 ticks (its notes at 0 and 96 stay); the plug-in track's pattern 1 is 768
            prints = Path(folder) / "Song Prints"
            prints.mkdir()
            float_wav(prints / "T02 A2.wav", 44100, 2, 100)
            _, out = self.export(folder, "L:0.1=384", f"1@1={prints}/T02 A2.wav", "L:1.1=768")
            _, data = read(out / "Beat.xpj")
        second = next(s["value"] for s in data["sequences"] if s["key"] == 1)
        self.assertEqual(second["lengthPulses"], 7680)
        clip = next(c["value"] for c in second["trackClipMaps"][0] if c["key"] == "Drum 001")
        self.assertEqual([(e["time"], e["note"]["note"]) for e in clip["eventList"]["events"]], [(0, 36), (960, 37), (3840, 36), (4800, 37)])
        self.assertEqual(clip["endPulses"], 7680)
        for track in data["tracks"]:
            if track["program"]["type"] in (0, 1, 6):
                self.assertEqual((track["length"], track["lengthFollowsSequenceLength"]), (0, True))

    def test_warped_pad(self):
        with tempfile.TemporaryDirectory() as folder:
            _, out = self.export(folder, "0.0~1")
            _, data = read(out / "Beat.xpj")
            back = self.run_tool("--mpc-import", out / "Beat.xpj").stdout
        pad = data["tracks"][0]["program"]["drum"]["instruments"][0]
        self.assertEqual((pad["warpEnable"], pad["bpmLock"], pad["tempo"]), (True, True, 72.5))
        self.assertIn("pad A01 file=Kick start=10 end=90 level=100 pan=0 pitch=0 play=1 mono=1 group=2 filter=Off cutoff=100 warp=1", back)

    def test_nothing_to_export(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "Empty.mldp"
            path.write_bytes(b"")
            result = self.run_tool("--mpc3-export", path, folder, "X")
        self.assertEqual(result.returncode, 1)
        self.assertIn("error=", result.stdout)


if __name__ == "__main__":
    unittest.main()
