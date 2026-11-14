#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Make src/melodee/shell/mpc3_template.json from a project MPC 3 saved.

MPC 3 reads a project with every field its own save writes, so Melodee's MPC 3
export (shell/mpc3_export.cpp) fills a copy of a real save rather than writing
the document from nothing. This keeps the save's settings and drops its music:
one empty drum track, MIDI track and audio track to copy, one empty pad, the
Submix, Return and Out tracks, one sequence and one clip without events, and
one note and one audio event to copy. No samples, names or notes of the saved
project are kept.

    python3 tools/mpc3_template.py "~/Documents/MPC 3/Some Project.xpj"
"""
import copy
import gzip
import json
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "src/melodee/shell/mpc3_template.json"
DRUM, KEYGROUP, AUDIO, MIDI, RETURN, SUBMIX, OUTPUT = 0, 1, 6, 4, 7, 8, 9
NOTE_EVENT, AUDIO_EVENT = 3, 4


def read(path):
    text = gzip.decompress(Path(path).expanduser().read_bytes()).decode()
    lines = text.split("\n", 5)
    if lines[0] != "ACVS" or lines[2] != "SerialisableProjectData" or lines[3] != "json":
        sys.exit("not an MPC project: " + path)
    return lines[:5], json.loads(lines[5])["data"]


def first(items, test, what):
    for item in items:
        if test(item):
            return item
    sys.exit("the project has no " + what)


def no_inserts(node):
    """Every insert effect taken out: a template carries no plug-in state."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "inserts" and isinstance(value, dict):
                value["effects"] = []
            else:
                no_inserts(value)
    elif isinstance(node, list):
        for value in node:
            no_inserts(value)


def blank_layers(instrument):
    for layer in instrument["layersv"]:
        layer["sampleName"] = layer["sampleFile"] = ""
        layer["sliceInfo"]["Start"] = layer["sliceInfo"]["End"] = layer["sliceInfo"]["LoopStart"] = 0


def bare(track):
    track = copy.deepcopy(track)
    track["program"]["samples"] = []
    track["arrangementClipMap"] = []
    track["sharedClipMap"] = []
    return track


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    header, data = read(sys.argv[1])
    tracks = data["tracks"]
    kind = lambda t: t["program"]["type"]

    drum = bare(first(tracks, lambda t: kind(t) == DRUM, "drum track"))
    pads = drum["program"]["drum"]["instruments"]
    pad = copy.deepcopy(first(pads, lambda i: not any(l["sampleFile"] for l in i["layersv"]), "empty pad"))
    blank_layers(pad)
    drum["program"]["drum"]["instruments"] = []
    audio = bare(first(tracks, lambda t: kind(t) == AUDIO, "audio track"))
    midi = bare(first(tracks, lambda t: kind(t) == MIDI, "MIDI track"))
    buses = [copy.deepcopy(t) for t in tracks if kind(t) in (RETURN, SUBMIX, OUTPUT)]

    sequence = copy.deepcopy(data["sequences"][0]["value"])
    clips = sequence["trackClipMaps"][0]
    clip = copy.deepcopy(clips[0]["value"])
    events = [e for c in data["sequences"] for m in c["value"]["trackClipMaps"] for entry in m for e in entry["value"]["eventList"]["events"]]
    note = copy.deepcopy(first(events, lambda e: e["type"] == NOTE_EVENT, "note"))
    audio_event = copy.deepcopy(first(events, lambda e: e["type"] == AUDIO_EVENT, "audio event"))
    audio_event["audio"]["sample"]["name"] = audio_event["audio"]["sample"]["path"] = audio_event["audio"]["name"] = ""
    clip["eventList"]["events"] = []
    clip["name"] = ""
    sequence["trackClipMaps"] = []
    sequence["name"] = ""

    for key in ("tracks", "sequences"):
        del data[key]
    data["samples"] = []
    data["clipPlayerData"]["trackClipTransportMap"] = []
    data["songs"] = [{"name": "(unnamed)", "ignoreTempo": False, "items": []} for _ in data["songs"]]
    data["info"] = {key: "" for key in data["info"]}
    no_inserts(data)
    no_inserts(buses)
    no_inserts(drum)
    no_inserts(pad)

    template = {"header": header, "data": data, "drumTrack": drum, "drumPad": pad, "audioTrack": audio, "midiTrack": midi,
                "buses": buses, "sequence": sequence, "clip": clip, "noteEvent": note, "audioEvent": audio_event}
    # One value a line, so a new template shows as a readable diff; the binary carries it as it is.
    OUT.write_text(json.dumps(template, indent=0, separators=(",", ":")) + "\n")
    print(OUT, OUT.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
