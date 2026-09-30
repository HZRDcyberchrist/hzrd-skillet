# Skillet NTSC — videoskillet for Resolume

A Resolume FFGL effect that runs [videoskillet](https://github.com/cmdcolin/videoskillet)'s analog video engine on your layer. videoskillet is Colin Diesh's WebGPU NTSC emulator. It doesn't paint glitches over the picture: it encodes the frame into a real composite waveform (525 lines × 910 samples of voltage), damages the waveform the way tape, cables, RF and circuit bending do, then decodes it with a model of a TV that has to find sync in whatever it's handed. Dot crawl, rainbowing, tearing, rolling and feedback all fall out of that.

This is a port of the whole signal path, not an approximation: all 26 GPU passes and all 156 presets.

## Get the plugin

You need Resolume Arena or Avenue 7 or later on **Windows**, and a GPU with OpenGL 4.3 or newer (any discrete or integrated GPU from the last decade).

Build `SkilletNTSC.dll` one of two ways.

**GitHub Actions (no tools needed).** Put this folder in a GitHub repository and push. The `build` workflow compiles the DLL on Windows and attaches it to the run as the artifact `SkilletNTSC-windows-x64`.

**Visual Studio 2022 (with the "Desktop development with C++" workload, which includes CMake).** From a Developer PowerShell in this folder:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

The DLL lands in `build\Release\SkilletNTSC.dll`.

## Install

1. Copy `SkilletNTSC.dll` into `Documents\Resolume Arena\Extra Effects` (or `Documents\Resolume Avenue\Extra Effects`).
2. Restart Resolume.
3. Drag **Skillet NTSC** from the Effects browser onto a layer or clip.

## Controls

**Preset**
| Control | What it does |
|---|---|
| Preset | The full catalogue of 156 looks, grouped as in the app (Tape wear, RF / Broadcast, Feedback loops, Circuit bent…) |
| Morph | Seconds a preset change glides over, like the app's morph. At 0 changes cut |
| Prev / Next / Random | Step through or jump around the catalogue |
| Amount | 0 is a clean signal, 100% is the preset as authored, up to 200% exaggerates it |

**Favorites**: your own shortlist of presets, with its own dropdown and buttons.
| Control | What it does |
|---|---|
| Add favorite / Remove favorite | Adds the preset that's up to your favorites, or takes it off. Favorites get a `<3` in the Preset and Pad preset dropdowns |
| Favorites | Your favorites, in the order you added them. Shows "(not a favorite)" when the preset that's up isn't one |
| Fav prev / Fav next / Fav random | Step through or jump around your favorites only |

Favorites are kept in `Documents\SkilletNTSC-favorites.txt`, so every copy of the effect shares them and they survive restarts and new compositions.

**Perform**: live knobs on top of whatever preset is up.
| Control | What it does |
|---|---|
| Noise | Adds RF noise |
| Tape wobble | Adds time-base flutter and wow |
| Tracking | VHS tracking error |
| Roll | Detunes the vertical oscillator and loosens hold, so the picture rolls either way |
| Bend | Horizontal deflection bend |
| Tint | The set's tint knob, ±180° |
| Color | Chroma gain, ×0 to ×4 |
| Camera loop | Points a camera at the tube (the app's camera feedback) |
| Time | Slows the whole simulation; 0 freezes it |

**Assign 1–4 / Knob 1–4**: pick any of the app's 281 controls in an Assign dropdown, then play it with the matching Knob. The knob sweeps that control's full range and overrides the preset while assigned.

**Sources**
| Control | What it does |
|---|---|
| Source A | The layer, or a generator in its place: TV static, blank-tape static, video synth |
| Source B | The mixer's second input. **Auto** feeds the layer itself to presets that mix two pictures. You can also pick off, TV static, blank-tape static, video synth, color bars, or layer copy |
| Mirror A | Flips the layer before it's encoded |
| Fill frame | Stretches the 4:3 tube to fill the output. Off is the authentic pillarbox |
| Reset signal | Clears everything the path carries between frames: feedback, phosphor, sync lock, tape |
| Caption | Text for the closed-caption decoder and the character generator presets |

**Pads 1–16**: each pad has a preset dropdown and a button that fires it. They start on a spread of looks from across the catalogue.

## Playing presets from a MIDI controller

Every control above can be mapped. In Resolume, turn on MIDI mapping (Shortcuts menu → Edit MIDI), click a control, then press or turn the thing on your controller that should drive it.

- **One pad per preset**: choose a preset in each "Pad N preset" dropdown, then map the **Pad N** buttons to your controller's pads. Pressing a pad morphs to its preset (set Morph to 0 for hard cuts).
- **Scroll the catalogue**: map a knob or fader to **Preset**.
- **Buttons**: map Prev, Next and Random to buttons.
- **Favorites**: star looks with Add favorite, then map Fav prev, Fav next and Fav random to buttons to play only those.
- **Knobs**: map Amount, the Perform knobs and Knob 1–4 to your controller's encoders or faders.

The plugin saves with your composition like any Resolume effect, so pad assignments come back when you reopen it.

## Worth knowing

- The simulation runs on a fixed 754 × 480 NTSC raster, like the real thing, and is scaled up to your output.
- The app's audio-reactive controls (audio bend, roll, tear and so on) have no audio input in the plugin yet, so they do nothing. Resolume's own audio-reactive automation on any parameter works.
- App features that sit around the engine, like the LFO bay, scenes, recorder and saved looks, aren't part of this. Resolume's dashboard, LFOs and automation cover the same ground.
- This is an effect with one input, so slot B comes from generators, bars or the layer itself. A two-input mixer version is possible.

## How it was built, and how to update it

- **Shaders**: `tools/wgsl2glsl.mjs` is a small WGSL → GLSL 4.5 compiler written for these shaders. It type-checks the WGSL and preserves WGSL semantics where GLSL differs: float `%`, `select()` argument order, vector comparisons, saturating conversions and zero-initialized variables. All 26 programs compile on Mesa, and their output was checked visually against the app's looks.
- **CPU side**: the per-frame signal state (sync servos, tape time-base walk, deck pause, RF drift, captions, filter design, uniform packing) is ported to C++ in `engine/`. `tools/golden/check_all.sh` runs every preset through the original TypeScript and the C++ port with the same random seed and requires identical bytes. All 156 presets match.
- **Tables**: the control schema, uniform layout and preset catalogue are generated from the videoskillet sources, not hand-copied.

To pull in a newer videoskillet (needs Node 22+ and Python 3 with Pillow):

```
VS=/path/to/videoskillet tools/regen.sh
VS=/path/to/videoskillet tools/golden/check_all.sh
```

`test/` has a headless harness (`skillet_render`) and a minimal FFGL host (`skillet_ffgl_host`) that loads the plugin through `plugMain` the way Resolume does. Both build on Linux with Mesa via the same CMake project.

## Credits

videoskillet is © 2026 Colin Diesh, MIT License. This port keeps that license (see `LICENSE`). The FFGL SDK is © FreeFrame / Resolume (BSD), and GLEW and the DejaVu font behind the caption ROM have their own notices. See `THIRD_PARTY_NOTICES.md`.
