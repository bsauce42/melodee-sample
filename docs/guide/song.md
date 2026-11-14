# Song

Previous: [Sequencing](sequencing.md) · [Contents](../GUIDE.md) · Next: [Looper](looper.md)

Press **SHIFT** + **Pad 12** to open **Song**. Press it again on Song to close
it. Song's own keys stay its actions: **B1** (**EXPORT**), **B2** (**MUTE**)
and **B3** (**PLACE**, or **DUPLICATE** on a clip). Each track has a lane.
Each lane holds clips of that track's patterns. The header shows the
cursor's bar and time, and the loop.

![Song](../images/screens/song.png)

| Control | Action |
| --- | --- |
| Encoder | Move the cursor by a bar. |
| Pads | Select a lane. An empty track opens Stage to make it. |
| **K1** | Zoom (4–64 bars). |
| **K2** / **K3** | The next clip's pattern and length, or the clip under the cursor. |
| **B3** | Place a clip. On a clip: duplicate it after itself. |
| **B2** | Mute or unmute the clip. |
| **SHIFT** + **B2** | The clip's Motion on or off. |
| **SHIFT** + **B3** | Make the clip's pattern its own copy. |
| Encoder push | Open the clip's notes: Roll, or Step Edit on a kit. On empty time, place a clip. |
| **CHOP** | Split the clip at the cursor. |
| **ERASE** | Delete the clip. |
| **SHIFT** + **ERASE** | Copy the clip, placed after itself. |
| **NOTE REPEAT** | Repeat the clip up to the next marker. |
| **LOOP** | Loop the clip's bars. |
| **SHIFT** + encoder push | Add a marker. On a marker: name it. |
| **−** / **+** | Jump between markers. |
| **PLAY** | Play the song from the cursor. |

When the song plays, patterns you launch are recorded as clips.

**Tempo lane**: **SHIFT** + **K1** sets the tempo from the cursor's bar.
**SHIFT** + **K2** sets the change as a step or a ramp.

![The tempo lane](../images/screens/songtempo.png)

## Export

Press **B1** (**EXPORT**):

- **B2** exports. Turn the encoder first to choose what: a **MIXDOWN**
  (32-bit float, 24-bit or 16-bit WAV), **STEMS** (one WAV per track and
  bus, plus the mix), a **MIDI FILE**, or an **MPC 3** project. The key
  shows the choice.
- **B3** (**PATTERN**) writes the song into new patterns, end to end.
- **B1** (**CANCEL**) goes back to the song.

![Export](../images/screens/songexport.png)

Exports go to `Melodee/Mixdowns`, `Melodee/Stems/<name>/` and
`Melodee/MIDI` on the card. A `.mid` file in the browser loads its notes
onto the selected track's pattern.

### MPC 3 project

**MPC 3** writes `Melodee/MPC 3/<name>.xpj` and its `<name>_[ProjectData]`
folder with the samples. MPC One, Live, Key, X and the MPC 3 software open it.

- Each kit track becomes a drum track. Its pads keep their samples, layers,
  trim, loop, tune, level, pan, mute group, envelope, filter and warp.
- A pad playing an instrument (SFZ, MPC keygroup program, EXS, DecentSampler),
  and a sample pad its patterns play at other pitches, becomes a keygroup
  track of its own. Its notes go with it.
- A plug-in track becomes an audio track of its prints. The export prints
  each pattern with notes first, skipping prints that are up to date. The
  project needs a file for its prints: save it first.
- Each pattern number becomes a sequence. The song becomes one sequence,
  **Song**, in the first free number, and MPC 3 opens on it.
- A MIDI track becomes an MPC MIDI track on its channel, with its bank and
  program. Pick its output port on the MPC.
- Effects, sends, the song's tempo changes and samples that are
  not WAV files are left out. The message after the export says what was.

---

Previous: [Sequencing](sequencing.md) · [Contents](../GUIDE.md) · Next: [Looper](looper.md)
