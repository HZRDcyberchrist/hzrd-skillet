#!/usr/bin/env bash
# Golden check: runs every preset through the real videoskillet TypeScript and
# through the C++ SignalChain with the same seed, and requires the packed
# uniforms, feed blocks, line parameters and filter bank to match byte for byte.
#   VS=/path/to/videoskillet tools/golden/check_all.sh [frames] [seed]
set -euo pipefail
cd "$(dirname "$0")"
: "${VS:?set VS to a videoskillet checkout}"
export VS
FRAMES=${1:-30}; SEED=${2:-4242}
NODE="node --experimental-transform-types --no-warnings --import ../register.mjs"
${CXX:-g++} -std=c++17 -O2 chain_dump.cpp ../../engine/signal.cpp ../../engine/chain.cpp -I../../engine -o chain_dump
$NODE all_names.mjs > .names
pass=0; fail=0
while read -r p; do
  $NODE ref_chain.mjs "$p" "$FRAMES" "$SEED" 1 > .ref
  ./chain_dump "$p" "$FRAMES" "$SEED" 1 > .cpp
  if cmp -s .ref .cpp; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFF $p"; fi
done < .names
rm -f .ref .cpp .names chain_dump
echo "golden: $pass presets match, $fail differ"
[ "$fail" -eq 0 ]
