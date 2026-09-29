#!/usr/bin/env bash
# Regenerates everything under engine/generated/ from a videoskillet checkout:
# the translated GLSL, the control/uniform/preset tables, the uniform packer
# and the caption font. Run it after pulling a newer videoskillet, then run
# the golden check (tools/golden/check_all.sh) before building.
#
#   VS=/path/to/videoskillet tools/regen.sh
#
# Needs Node 22+ (it imports the TypeScript sources directly) and Python 3
# with Pillow for the font.
set -euo pipefail
cd "$(dirname "$0")"
: "${VS:?set VS to a videoskillet checkout}"
export VS
NODE="node --experimental-transform-types --no-warnings --import ./register.mjs"
mkdir -p ../build ../engine/generated
$NODE dump_wgsl.mjs ../build/wgsl
node translate_all.mjs ../build/wgsl ../build/glsl
node gen_shaders.mjs ../build/glsl ../engine/generated/shaders.gen.h
$NODE gen_tables.mjs ../engine/generated/tables.gen.h
node gen_uniforms.mjs ../engine/generated/uniforms.gen.inc
python3 gen_caption_rom.py ../engine/generated/captionrom.gen.h >/dev/null
echo "regenerated from $(git -C "$VS" rev-parse --short HEAD 2>/dev/null || echo "$VS")"
