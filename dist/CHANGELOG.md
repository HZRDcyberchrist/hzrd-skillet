# HZRD//Skillet release history

## v1.2.5 — October 27, 2026

- Added **Keep alpha** (Sources, off by default): the layer's transparency now passes through Skillet instead of every output pixel being opaque. Effects earlier in the chain that cut holes (for example HZRD//PANEL's Reveal Layer Below) keep showing the layers underneath when Skillet runs after them. The mask follows the tube's geometry (4:3 fit, Fill frame, Mirror A, tube turn, CRT zoom and glass curve); signal effects that move the picture (roll, tearing, wobble) move the colour but not the mask. The cabinet and pillarbox stay opaque. Only applies when Source A is the layer.

## v1.2.4 — October 1, 2026

- Added live audio scope, frequency focus, reaction threshold and custom bands to both plugins.
- Consolidated all audio controls into one Audio group.
- Fixed scope startup/unload crashes, disruptive refreshes, zero-gain response and stale reactions after input disconnects.
- Added HZRD//Archive scope styling, SHFTR window icon, crisp transparent sigil, Confirm, always-on-top behavior and restoration of minimized Resolume.
- Preserved existing plugin IDs, routable controls, presets, pads, favorites and composition-compatible parameter names.

## v1.1.0

- Added Windows audio input selection, gain and input meter.
- Added Kick > roll / bloom, Level > tear and Wave > bend / color / picture routes.
- Enabled audio response in the bass smack and sound at the rails presets.

## v1.0.x

- Initial effect and mixer: full analog NTSC signal engine, 156 presets, 16 MIDI pads, favorites, Perform controls, assignable controls and selectable picture sources.
