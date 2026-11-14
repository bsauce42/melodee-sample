# SPDX-License-Identifier: AGPL-3.0-only
"""A project written as an MPC 3 project by the built app, then read back."""
import gzip
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_mpc_project import BINARY, instrument, note, project, wav, write


def read(path):
    lines = gzip.decompress(Path(path).read_bytes()).decode().split("\n", 5)
    return lines[:5], json.loads(lines[5])["data"]


@unittest.skipUnless(BINARY.is_file(), "build/melodee is not built")
class Mpc3Export(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True, timeout=120)

    def export(self, folder):
        source = Path(folder) / "in"
        data = source / "Song_[ProjectData]"
        data.mkdir(parents=True)
        (data / "Kick.wav").write_bytes(wav(44100))
        (data / "Snare.wav").write_bytes(wav(22050))
        path = write(source, "Song", project([instrument("Kick.wav", 10, 90, group=2),
                                              instrument("Snare.wav", 10, 40, volume=48 / 127, filter_type=3, cutoff=0.5, one_shot=False)]))
        out = Path(folder) / "out"
        out.mkdir()
        result = self.run_tool("--mpc3-export", path, out, "Beat")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout, out

    def test_document(self):
        with tempfile.TemporaryDirectory() as folder:
            report, out = self.export(folder)
            self.assertIn("exported tracks=1 sequences=2 pads=2 notes=3 samples=2 skipped_tracks=0 skipped_pads=0 missing=0", report)
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

    def test_nothing_to_export(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "Empty.mldp"
            path.write_bytes(b"")
            result = self.run_tool("--mpc3-export", path, folder, "X")
        self.assertEqual(result.returncode, 1)
        self.assertIn("error=", result.stdout)


if __name__ == "__main__":
    unittest.main()
