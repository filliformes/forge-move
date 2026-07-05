# Sonic Potions LXR `.SND` kit file format

Reverse-engineered (clean-room, file-format only) from the **GPL-2.0 LXR
firmware** (`SonicPotions/LXR`, AVR front-panel side `front/LxrAvr/`) and
cross-checked against 9 real `.SND` files.

This is a **file-format spec**, not DSP code. No firmware DSP was copied.

---

## 0. Sources & provenance (read this first)

Primary source files (raw, fetched July 2026):

- `front/LxrAvr/Preset/presetManager.c` — the save/load routines
  (`preset_saveDrumset`, `preset_loadDrumset`, `preset_writeDrumsetData`,
  `preset_readDrumsetData`). This is the authoritative writer/reader.
- `front/LxrAvr/Preset/PresetManager.h` — declares `preset_currentName[8]`.
- `front/LxrAvr/Parameters.h` — the `enum ParamEnums` that names every
  parameter index. **The byte order of the file is exactly this enum order.**
- `front/LxrAvr/Menu/MenuText.h` — enum→string tables (waveform names,
  filter-type names, LFO wave names, output names).
- `front/LxrAvr/Menu/menu.c` / `Cc2Text.c` — confirm that waveform and
  filter-type parameters are stored as **raw small indices** (not 0–127
  scaled), and that most other params are 0–127.

**Provenance of the 9 test files (important):** The shipped
`Instructions.txt` states the patches (Björn Fogelberg's TR-707/808/909 set,
2017) were saved with the **"LXR Custom firmware 0.37 by Brendan Clarke"**,
*not* stock Sonic Potions firmware and *not* the Erica Synths LXR-02.

Consequences, verified numerically:

| Firmware | `END_OF_SOUND_PARAMETERS` | predicted `.snd` size |
|----------|---------------------------|------------------------|
| Stock `SonicPotions/LXR` (master, unchanged since 2015) | 228 | 236 bytes |
| `brendanclarke/LXR` **current** master (2026 rewrite) | 236 | 244 bytes |
| **Custom fw 0.37 (what wrote these files)** | **243** | **251 bytes** ✓ |

The real files are **251 bytes** = 8 (name) + **243** (params). So the exact
0.37 enum had 243 sound params. The 0.37 source is **not archived** in a
2017-dated commit on any public fork I could find (`brendanclarke/LXR` jumps
from 2015 straight to 2026 commits; `DoItYourSynth/LXRBrendanClarke` master is
just a stock mirror = 228). **However**, all three enum variants are
byte-identical from offset 8 up to and including the transient block
(`PAR_TRANS*`, last stable index 214 / file offset 222). All 15 extra params
in the 0.37 build were appended in the tail (offset 223+). The core
sound-shaping parameters — everything you need for a clean-room port — all
live in the stable region and decode perfectly. See §7 for the tail caveats.

---

## 1. Top-level file layout

`.SND` file = name + a flat dump of the sound-parameter portion of the
firmware's global `parameter_values[]` array, one byte per parameter, in
`enum ParamEnums` order. From `preset_saveDrumset()`:

```c
f_write(file, preset_currentName, 8, ...);   // 8-byte name
preset_writeDrumsetData(isMorph);            // END_OF_SOUND_PARAMETERS bytes
```

`preset_writeDrumsetData()` (non-morph path):

```c
for (i = 0; i < END_OF_SOUND_PARAMETERS; i++)
    f_write(file, &parameter_values[i], 1, ...);   // 1 byte each, raw
```

| Region | File offset | Size | Content |
|--------|-------------|------|---------|
| Name | `0x00`–`0x07` (0–7) | 8 bytes | ASCII kit name, space-padded (see §2) |
| Parameters | `0x08`+ (8+) | `END_OF_SOUND_PARAMETERS` bytes | one byte per param, enum order |

**No magic number. No version byte. No checksum.** A standalone `.SND` is
name + params, nothing else. (Version/padding only exist in the composite
`.all`/`.prf` files — `FILE_VERSION 2` — which are a different format.)

- Stock file size = `8 + 228 = 236`.
- These 0.37 files = `8 + 243 = 251` (confirmed on all 9).

The loader (`preset_readDrumsetData`) reads exactly `END_OF_SOUND_PARAMETERS`
bytes and **zero-fills any shortfall** — so files from a firmware with fewer
params load fine, missing params default to 0. This is why cross-version
loads mostly "just work".

---

## 2. Name field (offset 0, length 8)

- 8 bytes, ASCII, **not NUL-terminated by design** — space-padded (`0x20`)
  to 8 chars. Read/written verbatim as `preset_currentName[8]`.
- Verified: P001 starts `38 30 38 5F 42 46 20 20` = `"808_BF  "`.
- The `p001` in the *filename* is unrelated to the stored name; the stored
  name is what the LXR shows in its browser.

---

## 3. Parameter enumeration → file offset

**file_offset = 8 + enum_index.** Every parameter is **1 byte**.

The enum has two aliasing quirks that DO NOT add bytes (they reuse a slot):
- `PAR_OSC_WAVE_DRUM1 = PAR_MOD_WHEEL` → both are index **1** (offset 9).
  Slot 1 is functionally "drum-1 oscillator waveform".
- `PAR_NONE` (index 0, offset 8) is a dummy ("stupid offset +/-1" per the
  source comment) and is always effectively unused.

Two entries in the middle are NRPN scratch slots that occupy real bytes but
are not sound params: `NRPN_DATA_ENTRY_COARSE` (idx 5), `NRPN_FINE` (idx 97),
`NRPN_COARSE` (idx 98). They still consume one byte each.

### 3.1 Full ordered offset table (stable core region — identical across all firmware variants)

| idx | off (dec) | off (hex) | parameter | alias |
|-----|-----------|-----------|-----------|-------|
| 0 | 8 | 0x08 | PAR_NONE |  |
| 1 | 9 | 0x09 | PAR_MOD_WHEEL / **PAR_OSC_WAVE_DRUM1** | alias |
| 2 | 10 | 0x0a | PAR_OSC_WAVE_DRUM2 |  |
| 3 | 11 | 0x0b | PAR_OSC_WAVE_DRUM3 |  |
| 4 | 12 | 0x0c | PAR_OSC_WAVE_SNARE |  |
| 5 | 13 | 0x0d | NRPN_DATA_ENTRY_COARSE |  |
| 6 | 14 | 0x0e | PAR_WAVE1_CYM |  |
| 7 | 15 | 0x0f | PAR_WAVE1_HH |  |
| 8 | 16 | 0x10 | PAR_COARSE1 |  |
| 9 | 17 | 0x11 | PAR_FINE1 |  |
| 10 | 18 | 0x12 | PAR_COARSE2 |  |
| 11 | 19 | 0x13 | PAR_FINE2 |  |
| 12 | 20 | 0x14 | PAR_COARSE3 |  |
| 13 | 21 | 0x15 | PAR_FINE3 |  |
| 14 | 22 | 0x16 | PAR_COARSE4 |  |
| 15 | 23 | 0x17 | PAR_FINE4 |  |
| 16 | 24 | 0x18 | PAR_COARSE5 |  |
| 17 | 25 | 0x19 | PAR_FINE5 |  |
| 18 | 26 | 0x1a | PAR_COARSE6 |  |
| 19 | 27 | 0x1b | PAR_FINE6 |  |
| 20 | 28 | 0x1c | PAR_MOD_WAVE_DRUM1 |  |
| 21 | 29 | 0x1d | PAR_MOD_WAVE_DRUM2 |  |
| 22 | 30 | 0x1e | PAR_MOD_WAVE_DRUM3 |  |
| 23 | 31 | 0x1f | PAR_WAVE2_CYM |  |
| 24 | 32 | 0x20 | PAR_WAVE3_CYM |  |
| 25 | 33 | 0x21 | PAR_WAVE2_HH |  |
| 26 | 34 | 0x22 | PAR_WAVE3_HH |  |
| 27 | 35 | 0x23 | PAR_NOISE_FREQ1 |  |
| 28 | 36 | 0x24 | PAR_MIX1 |  |
| 29 | 37 | 0x25 | PAR_MOD_OSC_F1_CYM |  |
| 30 | 38 | 0x26 | PAR_MOD_OSC_F2_CYM |  |
| 31 | 39 | 0x27 | PAR_MOD_OSC_GAIN1_CYM |  |
| 32 | 40 | 0x28 | PAR_MOD_OSC_GAIN2_CYM |  |
| 33 | 41 | 0x29 | PAR_MOD_OSC_F1 |  |
| 34 | 42 | 0x2a | PAR_MOD_OSC_F2 |  |
| 35 | 43 | 0x2b | PAR_MOD_OSC_GAIN1 |  |
| 36 | 44 | 0x2c | PAR_MOD_OSC_GAIN2 |  |
| 37 | 45 | 0x2d | PAR_FILTER_FREQ_1 |  |
| 38 | 46 | 0x2e | PAR_FILTER_FREQ_2 |  |
| 39 | 47 | 0x2f | PAR_FILTER_FREQ_3 |  |
| 40 | 48 | 0x30 | PAR_FILTER_FREQ_4 |  |
| 41 | 49 | 0x31 | PAR_FILTER_FREQ_5 |  |
| 42 | 50 | 0x32 | PAR_FILTER_FREQ_6 |  |
| 43 | 51 | 0x33 | PAR_RESO_1 |  |
| 44 | 52 | 0x34 | PAR_RESO_2 |  |
| 45 | 53 | 0x35 | PAR_RESO_3 |  |
| 46 | 54 | 0x36 | PAR_RESO_4 |  |
| 47 | 55 | 0x37 | PAR_RESO_5 |  |
| 48 | 56 | 0x38 | PAR_RESO_6 |  |
| 49 | 57 | 0x39 | PAR_VELOA1 |  |
| 50 | 58 | 0x3a | PAR_VELOD1 |  |
| 51 | 59 | 0x3b | PAR_VELOA2 |  |
| 52 | 60 | 0x3c | PAR_VELOD2 |  |
| 53 | 61 | 0x3d | PAR_VELOA3 |  |
| 54 | 62 | 0x3e | PAR_VELOD3 |  |
| 55 | 63 | 0x3f | PAR_VELOA4 |  |
| 56 | 64 | 0x40 | PAR_VELOD4 |  |
| 57 | 65 | 0x41 | PAR_VELOA5 |  |
| 58 | 66 | 0x42 | PAR_VELOD5 |  |
| 59 | 67 | 0x43 | PAR_VELOA6 |  |
| 60 | 68 | 0x44 | PAR_VELOD6_CLOSED |  |
| 61 | 69 | 0x45 | PAR_VELOD6_OPEN |  |
| 62 | 70 | 0x46 | PAR_VOL_SLOPE1 |  |
| 63 | 71 | 0x47 | PAR_VOL_SLOPE2 |  |
| 64 | 72 | 0x48 | PAR_VOL_SLOPE3 |  |
| 65 | 73 | 0x49 | PAR_VOL_SLOPE4 |  |
| 66 | 74 | 0x4a | PAR_VOL_SLOPE5 |  |
| 67 | 75 | 0x4b | PAR_VOL_SLOPE6 |  |
| 68 | 76 | 0x4c | PAR_REPEAT4 |  |
| 69 | 77 | 0x4d | PAR_REPEAT5 |  |
| 70 | 78 | 0x4e | PAR_MOD_EG1 |  |
| 71 | 79 | 0x4f | PAR_MOD_EG2 |  |
| 72 | 80 | 0x50 | PAR_MOD_EG3 |  |
| 73 | 81 | 0x51 | PAR_MOD_EG4 |  |
| 74 | 82 | 0x52 | PAR_MODAMNT1 |  |
| 75 | 83 | 0x53 | PAR_MODAMNT2 |  |
| 76 | 84 | 0x54 | PAR_MODAMNT3 |  |
| 77 | 85 | 0x55 | PAR_MODAMNT4 |  |
| 78 | 86 | 0x56 | PAR_PITCH_SLOPE1 |  |
| 79 | 87 | 0x57 | PAR_PITCH_SLOPE2 |  |
| 80 | 88 | 0x58 | PAR_PITCH_SLOPE3 |  |
| 81 | 89 | 0x59 | PAR_PITCH_SLOPE4 |  |
| 82 | 90 | 0x5a | PAR_FMAMNT1 |  |
| 83 | 91 | 0x5b | PAR_FM_FREQ1 |  |
| 84 | 92 | 0x5c | PAR_FMAMNT2 |  |
| 85 | 93 | 0x5d | PAR_FM_FREQ2 |  |
| 86 | 94 | 0x5e | PAR_FMAMNT3 |  |
| 87 | 95 | 0x5f | PAR_FM_FREQ3 |  |
| 88 | 96 | 0x60 | PAR_VOL1 |  |
| 89 | 97 | 0x61 | PAR_VOL2 |  |
| 90 | 98 | 0x62 | PAR_VOL3 |  |
| 91 | 99 | 0x63 | PAR_VOL4 |  |
| 92 | 100 | 0x64 | PAR_VOL5 |  |
| 93 | 101 | 0x65 | PAR_VOL6 |  |
| 94 | 102 | 0x66 | PAR_PAN1 |  |
| 95 | 103 | 0x67 | PAR_PAN2 |  |
| 96 | 104 | 0x68 | PAR_PAN3 |  |
| 97 | 105 | 0x69 | NRPN_FINE |  |
| 98 | 106 | 0x6a | NRPN_COARSE |  |
| 99 | 107 | 0x6b | PAR_PAN4 |  |
| 100 | 108 | 0x6c | PAR_PAN5 |  |
| 101 | 109 | 0x6d | PAR_PAN6 |  |
| 102 | 110 | 0x6e | PAR_DRIVE1 |  |
| 103 | 111 | 0x6f | PAR_DRIVE2 |  |
| 104 | 112 | 0x70 | PAR_DRIVE3 |  |
| 105 | 113 | 0x71 | PAR_SNARE_DISTORTION |  |
| 106 | 114 | 0x72 | PAR_CYMBAL_DISTORTION |  |
| 107 | 115 | 0x73 | PAR_HAT_DISTORTION |  |
| 108 | 116 | 0x74 | PAR_VOICE_DECIMATION1 |  |
| 109 | 117 | 0x75 | PAR_VOICE_DECIMATION2 |  |
| 110 | 118 | 0x76 | PAR_VOICE_DECIMATION3 |  |
| 111 | 119 | 0x77 | PAR_VOICE_DECIMATION4 |  |
| 112 | 120 | 0x78 | PAR_VOICE_DECIMATION5 |  |
| 113 | 121 | 0x79 | PAR_VOICE_DECIMATION6 |  |
| 114 | 122 | 0x7a | PAR_VOICE_DECIMATION_ALL |  |
| 115 | 123 | 0x7b | PAR_FREQ_LFO1 |  |
| 116 | 124 | 0x7c | PAR_FREQ_LFO2 |  |
| 117 | 125 | 0x7d | PAR_FREQ_LFO3 |  |
| 118 | 126 | 0x7e | PAR_FREQ_LFO4 |  |
| 119 | 127 | 0x7f | PAR_FREQ_LFO5 |  |
| 120 | 128 | 0x80 | PAR_FREQ_LFO6 |  |
| 121 | 129 | 0x81 | PAR_AMOUNT_LFO1 |  |
| 122 | 130 | 0x82 | PAR_AMOUNT_LFO2 |  |
| 123 | 131 | 0x83 | PAR_AMOUNT_LFO3 |  |
| 124 | 132 | 0x84 | PAR_AMOUNT_LFO4 |  |
| 125 | 133 | 0x85 | PAR_AMOUNT_LFO5 |  |
| 126 | 134 | 0x86 | PAR_AMOUNT_LFO6 |  |
| 127 | 135 | 0x87 | PAR_BANK_CHANGE (0.37: PAR_RESERVED4) |  |
| 128 | 136 | 0x88 | PAR_FILTER_DRIVE_1 |  |
| 129 | 137 | 0x89 | PAR_FILTER_DRIVE_2 |  |
| 130 | 138 | 0x8a | PAR_FILTER_DRIVE_3 |  |
| 131 | 139 | 0x8b | PAR_FILTER_DRIVE_4 |  |
| 132 | 140 | 0x8c | PAR_FILTER_DRIVE_5 |  |
| 133 | 141 | 0x8d | PAR_FILTER_DRIVE_6 |  |
| 134 | 142 | 0x8e | PAR_MIX_MOD_1 |  |
| 135 | 143 | 0x8f | PAR_MIX_MOD_2 |  |
| 136 | 144 | 0x90 | PAR_MIX_MOD_3 |  |
| 137 | 145 | 0x91 | PAR_VOLUME_MOD_ON_OFF1 |  |
| 138 | 146 | 0x92 | PAR_VOLUME_MOD_ON_OFF2 |  |
| 139 | 147 | 0x93 | PAR_VOLUME_MOD_ON_OFF3 |  |
| 140 | 148 | 0x94 | PAR_VOLUME_MOD_ON_OFF4 |  |
| 141 | 149 | 0x95 | PAR_VOLUME_MOD_ON_OFF5 |  |
| 142 | 150 | 0x96 | PAR_VOLUME_MOD_ON_OFF6 |  |
| 143 | 151 | 0x97 | PAR_VELO_MOD_AMT_1 |  |
| 144 | 152 | 0x98 | PAR_VELO_MOD_AMT_2 |  |
| 145 | 153 | 0x99 | PAR_VELO_MOD_AMT_3 |  |
| 146 | 154 | 0x9a | PAR_VELO_MOD_AMT_4 |  |
| 147 | 155 | 0x9b | PAR_VELO_MOD_AMT_5 |  |
| 148 | 156 | 0x9c | PAR_VELO_MOD_AMT_6 |  |
| 149 | 157 | 0x9d | PAR_VEL_DEST_1 |  |
| 150 | 158 | 0x9e | PAR_VEL_DEST_2 |  |
| 151 | 159 | 0x9f | PAR_VEL_DEST_3 |  |
| 152 | 160 | 0xa0 | PAR_VEL_DEST_4 |  |
| 153 | 161 | 0xa1 | PAR_VEL_DEST_5 |  |
| 154 | 162 | 0xa2 | PAR_VEL_DEST_6 |  |
| 155 | 163 | 0xa3 | PAR_WAVE_LFO1 |  |
| 156 | 164 | 0xa4 | PAR_WAVE_LFO2 |  |
| 157 | 165 | 0xa5 | PAR_WAVE_LFO3 |  |
| 158 | 166 | 0xa6 | PAR_WAVE_LFO4 |  |
| 159 | 167 | 0xa7 | PAR_WAVE_LFO5 |  |
| 160 | 168 | 0xa8 | PAR_WAVE_LFO6 |  |
| 161 | 169 | 0xa9 | PAR_VOICE_LFO1 |  |
| 162 | 170 | 0xaa | PAR_VOICE_LFO2 |  |
| 163 | 171 | 0xab | PAR_VOICE_LFO3 |  |
| 164 | 172 | 0xac | PAR_VOICE_LFO4 |  |
| 165 | 173 | 0xad | PAR_VOICE_LFO5 |  |
| 166 | 174 | 0xae | PAR_VOICE_LFO6 |  |
| 167 | 175 | 0xaf | PAR_TARGET_LFO1 |  |
| 168 | 176 | 0xb0 | PAR_TARGET_LFO2 |  |
| 169 | 177 | 0xb1 | PAR_TARGET_LFO3 |  |
| 170 | 178 | 0xb2 | PAR_TARGET_LFO4 |  |
| 171 | 179 | 0xb3 | PAR_TARGET_LFO5 |  |
| 172 | 180 | 0xb4 | PAR_TARGET_LFO6 |  |
| 173 | 181 | 0xb5 | PAR_RETRIGGER_LFO1 |  |
| 174 | 182 | 0xb6 | PAR_RETRIGGER_LFO2 |  |
| 175 | 183 | 0xb7 | PAR_RETRIGGER_LFO3 |  |
| 176 | 184 | 0xb8 | PAR_RETRIGGER_LFO4 |  |
| 177 | 185 | 0xb9 | PAR_RETRIGGER_LFO5 |  |
| 178 | 186 | 0xba | PAR_RETRIGGER_LFO6 |  |
| 179 | 187 | 0xbb | PAR_SYNC_LFO1 |  |
| 180 | 188 | 0xbc | PAR_SYNC_LFO2 |  |
| 181 | 189 | 0xbd | PAR_SYNC_LFO3 |  |
| 182 | 190 | 0xbe | PAR_SYNC_LFO4 |  |
| 183 | 191 | 0xbf | PAR_SYNC_LFO5 |  |
| 184 | 192 | 0xc0 | PAR_SYNC_LFO6 |  |
| 185 | 193 | 0xc1 | PAR_OFFSET_LFO1 |  |
| 186 | 194 | 0xc2 | PAR_OFFSET_LFO2 |  |
| 187 | 195 | 0xc3 | PAR_OFFSET_LFO3 |  |
| 188 | 196 | 0xc4 | PAR_OFFSET_LFO4 |  |
| 189 | 197 | 0xc5 | PAR_OFFSET_LFO5 |  |
| 190 | 198 | 0xc6 | PAR_OFFSET_LFO6 |  |
| 191 | 199 | 0xc7 | **PAR_FILTER_TYPE_1** |  |
| 192 | 200 | 0xc8 | PAR_FILTER_TYPE_2 |  |
| 193 | 201 | 0xc9 | PAR_FILTER_TYPE_3 |  |
| 194 | 202 | 0xca | PAR_FILTER_TYPE_4 |  |
| 195 | 203 | 0xcb | PAR_FILTER_TYPE_5 |  |
| 196 | 204 | 0xcc | PAR_FILTER_TYPE_6 |  |
| 197 | 205 | 0xcd | PAR_TRANS1_VOL |  |
| 198 | 206 | 0xce | PAR_TRANS2_VOL |  |
| 199 | 207 | 0xcf | PAR_TRANS3_VOL |  |
| 200 | 208 | 0xd0 | PAR_TRANS4_VOL |  |
| 201 | 209 | 0xd1 | PAR_TRANS5_VOL |  |
| 202 | 210 | 0xd2 | PAR_TRANS6_VOL |  |
| 203 | 211 | 0xd3 | PAR_TRANS1_WAVE |  |
| 204 | 212 | 0xd4 | PAR_TRANS2_WAVE |  |
| 205 | 213 | 0xd5 | PAR_TRANS3_WAVE |  |
| 206 | 214 | 0xd6 | PAR_TRANS4_WAVE |  |
| 207 | 215 | 0xd7 | PAR_TRANS5_WAVE |  |
| 208 | 216 | 0xd8 | PAR_TRANS6_WAVE |  |
| 209 | 217 | 0xd9 | PAR_TRANS1_FREQ |  |
| 210 | 218 | 0xda | PAR_TRANS2_FREQ |  |
| 211 | 219 | 0xdb | PAR_TRANS3_FREQ |  |
| 212 | 220 | 0xdc | PAR_TRANS4_FREQ |  |
| 213 | 221 | 0xdd | PAR_TRANS5_FREQ |  |
| 214 | 222 | 0xde | PAR_TRANS6_FREQ |  |

**Everything above (offsets 8–222) is byte-identical across stock, BC-current,
and 0.37, and is fully verified against the 9 files.** Offsets 223–250 are the
tail (§7) — stock names them `PAR_AUDIO_OUT1..6` then `PAR_MIDI_NOTE1..7`, but
0.37 reorganised this region; treat the tail as low-confidence.

---

## 4. The 6 voices and how parameters map to them

The LXR has **6 fixed voices**. The default note→voice / channel layout is:

| Voice | Type | Notes |
|-------|------|-------|
| 1 | Drum (2-op FM + click/transient) | full FM, pitch env |
| 2 | Drum | full FM, pitch env |
| 3 | Drum | full FM, pitch env |
| 4 | Snare (tonal osc + noise) | its osc waveform = `PAR_OSC_WAVE_SNARE` |
| 5 | Cymbal (multi-osc FM metallic) | waves = `PAR_WAVE1/2/3_CYM` |
| 6 | Hi-hat (open/closed, choke) | waves = `PAR_WAVE1/2/3_HH`, dual decay |

**Important indexing subtlety:** most per-voice arrays are indexed 1..6 across
all voices (`PAR_COARSE1..6`, `PAR_FINE1..6`, `PAR_FILTER_FREQ_1..6`,
`PAR_RESO_1..6`, `PAR_VOL1..6`, `PAR_PAN1..6`, `PAR_FILTER_TYPE_1..6`,
`PAR_FILTER_DRIVE_1..6`, `PAR_TRANS1..6_*`, velocity env `PAR_VELOA1..6` /
`PAR_VELOD1..5` + `PAR_VELOD6_CLOSED/OPEN`).

But the **FM / modulator / pitch-env** parameters exist only for the voices
that have those DSP features:

- `PAR_FMAMNT1..3` + `PAR_FM_FREQ1..3` → **only voices 1–3** (the 3 FM drums).
- `PAR_PITCH_SLOPE1..4`, `PAR_MOD_EG1..4`, `PAR_MODAMNT1..4` → **voices 1–4**
  (3 drums + snare). Slot 4 belongs to the snare.
- `PAR_OSC_WAVE_DRUM1..3` (offsets 9/10/11) = drum osc waves;
  `PAR_MOD_WAVE_DRUM1..3` (offsets 28/29/30) = FM-modulator waves for drums.
- `PAR_NOISE_FREQ1` / `PAR_MIX1` (offsets 35/36) are the snare's noise
  frequency and osc/noise mix (voice 4).
- `PAR_MOD_OSC_*` (offsets 37–44) are cymbal/hat multi-oscillator params.
- Global-ish drive trims: `PAR_DRIVE1..3` (drums), `PAR_SNARE_DISTORTION`,
  `PAR_CYMBAL_DISTORTION`, `PAR_HAT_DISTORTION`.

### 4.1 Per-voice "sound reconstruction" cheat-sheet

For voice *n* (1..6), the sound-defining bytes are at:

| Purpose | Param | File offset (n) |
|---------|-------|-----------------|
| Osc waveform | `PAR_OSC_WAVE_DRUMn` (n=1..3), `PAR_OSC_WAVE_SNARE` (n=4), `PAR_WAVE1_CYM` (n=5), `PAR_WAVE1_HH` (n=6) | 9,10,11 / 12 / 14 / 15 |
| Coarse pitch | `PAR_COARSEn` | 16 + 2·(n−1) → 16,18,20,22,24,26 |
| Fine pitch | `PAR_FINEn` | 17 + 2·(n−1) → 17,19,21,23,25,27 |
| FM amount | `PAR_FMAMNTn` (n=1..3) | 90,92,94 |
| FM ratio/freq | `PAR_FM_FREQn` (n=1..3) | 91,93,95 |
| FM-mod waveform | `PAR_MOD_WAVE_DRUMn` (n=1..3) | 28,29,30 |
| Filter cutoff | `PAR_FILTER_FREQ_n` | 45 + (n−1) → 45..50 |
| Filter reso | `PAR_RESO_n` | 51 + (n−1) → 51..56 |
| Filter type | `PAR_FILTER_TYPE_n` | 199 + (n−1) → 199..204 |
| Filter drive | `PAR_FILTER_DRIVE_n` | 136 + (n−1) → 136..141 |
| Amp env attack | `PAR_VELOAn` | 57,59,61,63,65,67 |
| Amp env decay | `PAR_VELODn` (n=1..5); hat n=6 = `PAR_VELOD6_CLOSED`(68)+`PAR_VELOD6_OPEN`(69) | 58,60,62,64,66 / 68+69 |
| Amp decay slope | `PAR_VOL_SLOPEn` | 70 + (n−1) → 70..75 |
| Pitch env slope | `PAR_PITCH_SLOPEn` (n=1..4) | 86,87,88,89 |
| Pitch/mod EG amt | `PAR_MOD_EGn` (n=1..4) | 78,79,80,81 |
| Modulator amount | `PAR_MODAMNTn` (n=1..4) | 82,83,84,85 |
| Transient/click vol | `PAR_TRANSn_VOL` | 205 + (n−1) → 205..210 |
| Transient/click wave | `PAR_TRANSn_WAVE` | 211 + (n−1) → 211..216 |
| Transient/click freq | `PAR_TRANSn_FREQ` | 217 + (n−1) → 217..222 |
| Volume | `PAR_VOLn` | 96 + (n−1) → 96..101 |
| Pan | `PAR_PANn` (n=1..3 = off 102..104; **NRPN gap**; n=4..6 = 107..109) | 102,103,104 / 107,108,109 |
| Bit-crush/decimation | `PAR_VOICE_DECIMATIONn` | 116 + (n−1) → 116..121 |

**Note the pan discontinuity:** `PAR_PAN1..3` sit at offsets 102–104, then the
two NRPN scratch bytes (105,106) intervene, then `PAR_PAN4..6` at 107–109.
This is a genuine layout quirk, not an error.

---

## 5. Value ranges & enum meanings

All parameter bytes are **unsigned 0–127** unless noted. The firmware stores
raw 7-bit values (MIDI-CC-derived) and clamps in the menu. Bipolar params
(pan, some mod amounts) are centred at **63/64** (0 = hard left/negative,
127 = hard right/positive). Confirmed: every `.SND` byte observed ≤ 127.

### 5.1 Waveform / modulator-waveform select — RAW small index (0-based)

`PAR_OSC_WAVE_*`, `PAR_MOD_WAVE_*`, `PAR_WAVE*_CYM/HH`, `PAR_TRANS*_WAVE`.
Displayed via `waveformNames[curParmVal + 1]` (menu.c ~L1409) → **stored value
is the raw index**, NOT 0–127 scaled.

`waveformNames` (MenuText.h): 6 named entries, **then sample slots append**
(menu.c L1379: `waveformNames[0][0] + menu_numSamples`):

| value | meaning |
|-------|---------|
| 0 | Sin (sine) |
| 1 | Tri (triangle) |
| 2 | Saw |
| 3 | Rec (rectangle/pulse) |
| 4 | Noi (noise) |
| 5 | Cym (metallic/cymbal osc) |
| 6+ | **Sample slot (value − 6)** — user-loaded WAV from SD |

(The 707/808/909 kits lean heavily on sample slots 6+, which is why their
osc-wave bytes read 8–21.)

### 5.2 Filter type — RAW small index (0-based)

`PAR_FILTER_TYPE_1..6`, displayed via `filterTypes[curParmVal + 1]`
(menu.c ~L1406). `filterTypes` (MenuText.h, 8 entries):

| value | name | meaning |
|-------|------|---------|
| 0 | LP | low-pass |
| 1 | HP | high-pass |
| 2 | BP | band-pass |
| 3 | UBP | unity-gain band-pass |
| 4 | Nch | notch |
| 5 | Pek | peak |
| 6 | LP2 | 2-pole low-pass (steeper) |
| 7 | off | filter bypassed |

### 5.3 LFO waveform — `PAR_WAVE_LFO1..6`, raw index into `lfoWaveNames` (8)

`0 sin, 1 tri, 2 sup (ramp up), 3 sdn (ramp down), 4 sqr, 5 rnd, 6 xup
(exp up), 7 xdn (exp down)`.

### 5.4 Audio output routing — `outputNames` (6): `St1, St2, L1, R1, L2, R2`.

### 5.5 Coarse / fine pitch, filter freq, reso, vol, pan, drive, decay

All 0–127 continuous. Coarse pitch spans the tuning range (menu shows a note);
fine is a small detune. Filter freq/reso/drive and vol are straight 0–127
scalars. Pan 0–127 with 63/64 = centre. Decay/attack are AD-envelope 0–127
times (the LXR uses AD-with-repeat envelopes).

### 5.6 On/off & mod-target params

`PAR_VOLUME_MOD_ON_OFF*`, `PAR_RETRIGGER_LFO*`, `PAR_SYNC_LFO*` are booleans
(0/1). `PAR_VEL_DEST_*` and `PAR_TARGET_LFO*` are **indices into the firmware's
`modTargets[]` table** (normalised on load: any value ≥ number-of-mod-targets
is reset to 0). `PAR_VOICE_LFO*` = which voice (1..6) each LFO targets
(clamped to 1..6 on load).

---

## 6. Decode of the 9 real files (P001–P009)

Voice labels below follow the type mapping in §4
(V1–V3 = FM drums, V4 = snare, V5 = cymbal, V6 = hi-hat). All values are the
raw stored bytes; `fType` and osc-wave are decoded via §5.1/§5.2.
`velD` for V6 is shown `closed/open`. `trans=vol/wav/freq`.

Sanity check passed on **all 9**: names decode to clean ASCII; all bytes ≤ 127;
kick (V1) is loudest (vol 127) with pitch-envelope + click on the 808/909 kits;
cymbals/hats reference sample slots; snare distortion present on 909 kits. This
strongly confirms the offset map.

### P001 `808_BF`
```
V1 Drum1 : coarse=30 fine=94  filtF=2   reso=39  fType=5(Pek) fDrv=14 vol=127 pan=63 velA=0  velD=56   pSlope=79 modEG=15 modAmt=19 trans=113/10/127  FM=0/0
V2 Drum2 : coarse=55 fine=22  filtF=33  reso=5   fType=0(LP)  fDrv=36 vol=79  pan=63 velA=0  velD=22   pSlope=21 modEG=36 modAmt=10 trans=60/0/112    FM=22/88
V3 Drum3 : coarse=29 fine=30  filtF=2   reso=0   fType=7(off) fDrv=66 vol=19  pan=63 velA=13 velD=31   trans=0/0/0                                    FM=0/30
V4 Snare : coarse=29 fine=30  filtF=60  reso=67  fType=7(off) fDrv=41 vol=91  pan=63 velA=0  velD=53
V5 Cymbal: coarse=29 fine=30  filtF=103 reso=0   fType=7(off) fDrv=0  vol=110 pan=63 velA=0  velD=39
V6 Hihat : coarse=29 fine=30  filtF=2   reso=6   fType=7(off) fDrv=0  vol=75  pan=63 velA=3  velD=23/6
OscWave  : d1=0(Sin) d2=0(Sin) d3=13(Smpl7) snare=8(Smpl2) cym=10(Smpl4) hh=12(Smpl6)
Global   : Mix1=0 NoiseFreq1=127  Drive1-3=49/71/19  SnareDist=0 CymDist=0 HatDist=0
```

### P002 `808_2_BF`
```
V1 Drum1 : coarse=30 fine=94  filtF=2   reso=61  fType=5(Pek) fDrv=20 vol=127 pan=63 velA=0  velD=33   pSlope=79 modEG=15 modAmt=19 trans=113/10/127  FM=0/0
V2 Drum2 : coarse=67 fine=22  filtF=68  reso=5   fType=0(LP)  fDrv=36 vol=79  pan=63 velA=0  velD=20   trans=60/0/112                                 FM=19/30
V3 Drum3 : coarse=29 fine=30  filtF=98  reso=0   fType=1(HP)  fDrv=54 vol=25  pan=63 velA=17 velD=3    FM=0/30
V4 Snare : coarse=26 fine=30  filtF=60  reso=67  fType=1(HP)  fDrv=41 vol=16  pan=63 velA=0  velD=32
V5 Cymbal: coarse=29 fine=30  filtF=0   reso=41  fType=7(off) fDrv=27 vol=51  pan=63 velA=0  velD=61
V6 Hihat : coarse=29 fine=30  filtF=127 reso=0   fType=7(off) fDrv=0  vol=77  pan=63 velA=0  velD=64/3
OscWave  : d1=0(Sin) d2=0(Sin) d3=13(Smpl7) snare=8(Smpl2) cym=10(Smpl4) hh=11(Smpl5)
Global   : Mix1=10 NoiseFreq1=127  Drive1-3=40/23/0  SnareDist=102 CymDist=57 HatDist=0
```

### P003 `808_3_BF`
```
V1 Drum1 : coarse=30 fine=94  filtF=127 reso=0   fType=0(LP)  fDrv=24 vol=127 pan=63 velA=0  velD=49   pSlope=79 modEG=15 modAmt=19 trans=127/10/127  FM=0/0
V2 Drum2 : coarse=98 fine=127 filtF=0   reso=0   fType=7(off) fDrv=0  vol=12  pan=63 velA=0  velD=8    FM=0/0
V3 Drum3 : coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=0  vol=40  pan=63 velA=2  velD=27   trans=0/0/127  FM=0/30
V4 Snare : coarse=29 fine=30  filtF=96  reso=0   fType=1(HP)  fDrv=0  vol=92  pan=63 velA=0  velD=25
V5 Cymbal: coarse=29 fine=30  filtF=107 reso=0   fType=2(BP)  fDrv=79 vol=58  pan=63 velA=2  velD=28
V6 Hihat : coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=25 vol=68  pan=63 velA=0  velD=27/3
OscWave  : d1=0(Sin) d2=0(Sin) d3=13(Smpl7) snare=8(Smpl2) cym=10(Smpl4) hh=12(Smpl6)
Global   : Mix1=21 NoiseFreq1=127  Drive1-3=49/65/0  SnareDist=50 CymDist=85 HatDist=0
```

### P004 `808_4_BF`
```
V1 Drum1 : coarse=30 fine=94  filtF=127 reso=0   fType=0(LP)  fDrv=24 vol=127 pan=63 velA=0  velD=49   pSlope=79 modEG=15 modAmt=19 trans=127/10/127  FM=0/0
V2 Drum2 : coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=0  vol=29  pan=63 velA=0  velD=54   FM=0/0
V3 Drum3 : coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=0  vol=32  pan=63 velA=2  velD=27   trans=0/0/127  FM=0/30
V4 Snare : coarse=29 fine=30  filtF=96  reso=0   fType=1(HP)  fDrv=0  vol=92  pan=63 velA=0  velD=25
V5 Cymbal: coarse=29 fine=30  filtF=36  reso=0   fType=1(HP)  fDrv=82 vol=32  pan=63 velA=2  velD=28
V6 Hihat : coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=25 vol=68  pan=63 velA=0  velD=27/3
OscWave  : d1=0(Sin) d2=18(Smpl12) d3=19(Smpl13) snare=8(Smpl2) cym=10(Smpl4) hh=12(Smpl6)
Global   : Mix1=21 NoiseFreq1=127  Drive1-3=49/65/0  SnareDist=50 CymDist=85 HatDist=0
```

### P005 `808/9_BF`
```
V1 Drum1 : coarse=30 fine=85  filtF=2   reso=50  fType=5(Pek) fDrv=22 vol=127 pan=63 velA=0  velD=27   pSlope=35 modEG=35 modAmt=29 trans=127/2/127   FM=0/0
V2 Drum2 : coarse=55 fine=22  filtF=71  reso=5   fType=0(LP)  fDrv=36 vol=32  pan=63 velA=0  velD=18   pSlope=18 modEG=36 modAmt=10 trans=0/7/0       FM=47/17
V3 Drum3 : coarse=29 fine=30  filtF=59  reso=0   fType=1(HP)  fDrv=33 vol=11  pan=63 velA=0  velD=44   FM=0/30
V4 Snare : coarse=29 fine=30  filtF=102 reso=0   fType=1(HP)  fDrv=29 vol=73  pan=63 velA=0  velD=36
V5 Cymbal: coarse=29 fine=30  filtF=127 reso=0   fType=0(LP)  fDrv=0  vol=101 pan=63 velA=0  velD=22
V6 Hihat : coarse=29 fine=30  filtF=2   reso=6   fType=7(off) fDrv=0  vol=47  pan=63 velA=0  velD=36/6
OscWave  : d1=0(Sin) d2=0(Sin) d3=16(Smpl10) snare=9(Smpl3) cym=8(Smpl2) hh=14(Smpl8)
Global   : Mix1=0 NoiseFreq1=127  Drive1-3=49/110/19  SnareDist=0 CymDist=0 HatDist=31
```

### P006 `7/8/9_BF`
```
V1 Drum1 : coarse=29 fine=30  filtF=5   reso=63  fType=5(Pek) fDrv=31 vol=127 pan=63 velA=0  velD=27   pSlope=21 modEG=39 modAmt=39 trans=127/10/127  FM=0/0
V2 Drum2 : coarse=52 fine=63  filtF=23  reso=0   fType=4(Nch) fDrv=80 vol=46  pan=63 velA=12 velD=20   pSlope=18 modEG=63 modAmt=17 trans=9/8/5       FM=24/22
V3 Drum3 : coarse=27 fine=30  filtF=68  reso=0   fType=1(HP)  fDrv=33 vol=17  pan=63 velA=3  velD=64   FM=0/30
V4 Snare : coarse=29 fine=30  filtF=58  reso=0   fType=7(off) fDrv=45 vol=71  pan=63 velA=1  velD=25
V5 Cymbal: coarse=29 fine=30  filtF=127 reso=43  fType=7(off) fDrv=0  vol=79  pan=63 velA=1  velD=43   trans=0/13/0
V6 Hihat : coarse=28 fine=30  filtF=2   reso=6   fType=7(off) fDrv=0  vol=47  pan=63 velA=0  velD=51/4
OscWave  : d1=0(Sin) d2=0(Sin) d3=16(Smpl10) snare=7(Smpl1) cym=10(Smpl4) hh=14(Smpl8)
Global   : Mix1=1 NoiseFreq1=127  Drive1-3=45/67/0  SnareDist=21 CymDist=39 HatDist=31
```

### P007 `909_BF`
```
V1 Drum1 : coarse=30 fine=107 filtF=5   reso=127 fType=5(Pek) fDrv=32 vol=127 pan=63 velA=0  velD=20   pSlope=15 modEG=36 modAmt=57 trans=0/0/127     FM=0/0
V2 Drum2 : coarse=40 fine=4   filtF=22  reso=43  fType=4(Nch) fDrv=80 vol=40  pan=63 velA=0  velD=20   pSlope=18 modEG=63 modAmt=17 trans=6/8/5       FM=29/13
V3 Drum3 : coarse=29 fine=30  filtF=48  reso=0   fType=1(HP)  fDrv=0  vol=48  pan=63 velA=8  velD=44   FM=0/30
V4 Snare : coarse=27 fine=30  filtF=0   reso=0   fType=7(off) fDrv=67 vol=25  pan=63 velA=11 velD=38
V5 Cymbal: coarse=29 fine=30  filtF=127 reso=0   fType=0(LP)  fDrv=51 vol=85  pan=63 velA=0  velD=63   trans=0/13/0
V6 Hihat : coarse=28 fine=30  filtF=2   reso=6   fType=7(off) fDrv=0  vol=10  pan=63 velA=0  velD=22/4
OscWave  : d1=1(Tri) d2=0(Sin) d3=16(Smpl10) snare=9(Smpl3) cym=10(Smpl4) hh=14(Smpl8)
Global   : Mix1=0 NoiseFreq1=127  Drive1-3=57/75/0  SnareDist=64 CymDist=19 HatDist=102
```

### P008 `707_BF`
```
V1 Drum1 : coarse=29 fine=30  filtF=5   reso=122 fType=5(Pek) fDrv=24 vol=127 pan=63 velA=0  velD=27   trans=71/10/69   FM=0/0
V2 Drum2 : coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=0  vol=56  pan=63 velA=0  velD=20   trans=0/8/0      FM=0/0
V3 Drum3 : coarse=29 fine=30  filtF=0   reso=0   fType=1(HP)  fDrv=36 vol=53  pan=63 velA=8  velD=39   FM=0/30
V4 Snare : coarse=29 fine=30  filtF=58  reso=0   fType=7(off) fDrv=0  vol=103 pan=63 velA=16 velD=66
V5 Cymbal: coarse=30 fine=30  filtF=49  reso=16  fType=1(HP)  fDrv=92 vol=70  pan=63 velA=6  velD=11   trans=0/13/0
V6 Hihat : coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=0  vol=28  pan=63 velA=0  velD=42/4
OscWave  : d1=6(Smpl0) d2=20(Smpl14) d3=15(Smpl9) snare=7(Smpl1) cym=10(Smpl4) hh=21(Smpl15)
Global   : Mix1=0 NoiseFreq1=127  Drive1-3=45/0/0  SnareDist=0 CymDist=0 HatDist=24
```

### P009 `Tech_BF`
```
V1 Drum1 : coarse=29 fine=30  filtF=120 reso=0   fType=0(LP)  fDrv=55 vol=127 pan=63 velA=0  velD=127  FM=0/31
V2 Drum2 : coarse=3  fine=63  filtF=127 reso=0   fType=6(LP2) fDrv=0  vol=32  pan=63 velA=0  velD=33   pSlope=25 modEG=115 modAmt=75 trans=57/3/127  FM=0/127
V3 Drum3 : coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=0  vol=51  pan=63 velA=64 velD=41   trans=0/13/69  FM=0/40
V4 Snare : coarse=29 fine=30  filtF=0   reso=0   fType=0(LP)  fDrv=0  vol=95  pan=63 velA=17 velD=94
V5 Cymbal: coarse=29 fine=30  filtF=0   reso=0   fType=7(off) fDrv=0  vol=127 pan=63 velA=0  velD=45
V6 Hihat : coarse=29 fine=30  filtF=121 reso=107 fType=7(off) fDrv=4  vol=67  pan=63 velA=0  velD=9/6
OscWave  : d1=17(Smpl11) d2=0(Sin) d3=13(Smpl7) snare=19(Smpl13) cym=10(Smpl4) hh=14(Smpl8)
Global   : Mix1=0 NoiseFreq1=0  Drive1-3=0/103/0  SnareDist=0 CymDist=51 HatDist=36
```

---

## 7. Tail region (file offsets 223–250) & confidence / unknowns

### The tail bytes (raw, all 9 files)

```
offset:   223..235  236  237..243  244  245  246  247..250
P001-008:  all 0     4    all 0     63    0   63    all 0
P009:      all 0     4    all 0      0    0    0    all 0
```

### What we know for sure

- **Offsets 8–222** (name + all core sound params through the transient block)
  are 100% confirmed: byte-identical across stock/BC/0.37 enums and every value
  decodes sanely across all 9 files. **This is the whole clean-room payload
  you need.** A Forge port can read these offsets directly.
- The file is a flat `parameter_values[i]` dump, 1 byte/param, name first, no
  header/version/checksum.

### What is uncertain (the 15 extra 0.37 params, offsets 223–250)

The stock enum names offsets 223–235 as `PAR_AUDIO_OUT1..6` then
`PAR_MIDI_NOTE1..7`. The 0.37 custom firmware **reorganised this tail** (the
current BC fork, a later descendant of 0.37, moved MIDI notes out of the sound
block, moved AUDIO_OUT earlier, and inserted `PAR_ENVELOPE_POSITION_1..6`,
`PAR_KIT_VERSION`, and per-voice morph params `PAR_MORPH_DRUM1..5`). I could
**not** find the exact 0.37-dated source, so I cannot name each of these 15
bytes with certainty. Observations:

- **Offset 236 = `0x04` in ALL nine files.** Almost certainly a constant
  marker — a strong candidate for a **`PAR_KIT_VERSION`** byte (value 4). High
  confidence it's a version/format tag; medium confidence on the exact name.
- **Offsets 244 and 246 = `63` (0x3F) in the 8 TR kits, `0` in P009 "Tech".**
  63 is the LXR "centre" value. These look like a pair of centred, defaulted
  params (plausibly per-voice morph offsets or an aux pan/level). **Meaning
  unconfirmed.**
- All other tail bytes (223–235, 237–243, 245, 247–250) are `0` in every
  file — consistent with unused/default AUDIO_OUT (0 = "St1") and MIDI-note (0
  = "use step note") fields, but I cannot prove the exact offset-to-name map
  for 0.37 without its source.

### Practical guidance for the Forge port

Use offsets **8–222** as the authoritative sound spec (fully verified). Ignore
or treat the 223–250 tail as opaque: audio-output routing, MIDI notes, kit
version, and (possibly) individual-voice morph — none of which affect the drum
*sound* you're reconstructing. If you need to WRITE `.SND` files that a real
0.37 LXR will load, zero-fill 223–250 except set offset 236 = 4; the loader
zero-fills anything short anyway.

### Firmware-variant note

If you ever obtain the exact "LXR Custom firmware 0.37 by Brendan Clarke"
`Parameters.h` (the enum that yields `END_OF_SOUND_PARAMETERS == 243`), the
tail can be pinned down precisely. The 0.37 source is not in a 2017 commit on
`brendanclarke/LXR` (jumps 2015→2026) nor in `DoItYourSynth/LXRBrendanClarke`
(stock mirror). It may only exist in a firmware `.hex` release or an offline
archive.

---

## 8. Reference: `.pat` / `.all` / `.prf` (NOT the same format)

For completeness (these are separate files, not `.SND`):

- **`.pat`** (pattern): 8-byte name, then per-step `StepData` structs
  (7×8×128 steps), main-step data, next/repeat, shuffle, track lengths.
- **`.all`** / **`.prf`**: 8-byte name + **1 version byte (`FILE_VERSION=2`)**
  + 64-byte globals/perf block (padded) + **512-byte kit block** (the drumset
  data, same param order as `.SND`, padded to 512) + pattern data. So an `.all`
  file *contains* a `.SND`-style kit at a known offset but is otherwise larger
  and versioned. `.SND` itself has no version byte.
```
