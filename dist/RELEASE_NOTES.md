# HZRD//Skillet v1.2.4 — Audio scope and focus

Tune how Skillet reacts to sound with a live scope, frequency focus and a reaction threshold. Available in both the effect and mixer.

## New audio controls

- **Audio scope** shows the incoming and selected waveforms, recent sound level and frequency spectrum.
- **Audio focus** selects Full mix, Kick, Bass, Mids, Highs or a Custom band. Kick routes follow attacks in the selected range; Level and Wave routes follow its loudness and waveform.
- **Reactive line** sets the minimum level needed to react. Adjust it in Resolume or drag the amber line in the scope.
- **Band low / Band high** set the Custom band's frequency range.

All 14 audio controls live together in one Audio group. Tuning controls save with your composition and support MIDI mapping. Full mix with Reactive line at 0% keeps the original response. Frequency focus can still respond to different instruments in the same range.

## Scope and fixes

- Black background, HZRD//Archive fonts and colors, SHFTR window icon, and HZRD logo with small white ARCHIVE text.
- Crisp, transparent sigil behind the tuning area, fitted completely inside the window without cropping or stretching.
- Always-on-top scope; activating or restoring it also restores minimized Resolume.
- **Confirm** keeps the current live settings and closes the viewer.
- Fixed scope startup/unload crashes and repeated redraws that disrupted menus. Focus and band settings now use Resolume's Audio controls.
- Audio gain at zero mutes every audio route. Disconnected inputs clear the reaction instead of leaving stale sound data.

Existing plugin IDs, routable control positions and defaults, presets, pads and favorites are preserved. The read-only Status section now follows Audio.

## Install or upgrade

Download **HZRD-Skillet-v1.2.4-windows.zip** below. Quit Resolume, unzip it and replace **both** DLLs in your existing Skillet installation folder. For a new installation, copy them into `Documents\Resolume Arena\Extra Effects` or `Documents\Resolume Avenue\Extra Effects`. Restart Resolume. Keep one installed copy of each plugin.

Windows 10/11 · Resolume Arena or Avenue 7+ · OpenGL 4.3+

Port of videoskillet by Colin Diesh, MIT License. Includes the Skillet effect and mixer.
