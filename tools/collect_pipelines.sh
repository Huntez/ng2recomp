#!/usr/bin/env bash
# The packaged pipeline seed (packaging/ng2-pipelines-seed.bin): headless test runs through
# Chapter 1 and Mission Mode, fighting, each recording the pipelines it makes, merged.
#
# usage: tools/collect_pipelines.sh <out_dir> [missions, e.g. "1 2 3" (default 1-12)]
#   JOBS=<n>   runs at a time (default 3)
# Needs the DLC installed in game_files/dlc (Mission Mode). The seed is recorded at the default
# 2x antialiasing; the game moves its multisampled pipelines to the player's setting.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=$(realpath -m "${1:?out dir}")
missions=${2:-$(seq 1 12)}
jobs=${JOBS:-3}
mkdir -p "$out"

# Fighting for <seconds> from <ms>: quick and strong attacks, shuriken, ninpo, jumps.
fight() {
  local from=$1 to=$2 s="" t i=0
  local buttons=(x x y x b y rb x a y)
  for ((t = from; t < to; t += 600)); do
    s="$s,$t:${buttons[i++ % ${#buttons[@]}]}"
  done
  printf '%s' "$s"
}

run() {  # run <name> <seconds> <pad script>
  local dir=$out/$1
  rm -rf "$dir"
  NG2_PAD_SCRIPT="${3#,}" "$root/tools/run-headless.sh" "$2" "$dir" >/dev/null 2>&1 || true
  echo "$1: $(( ($(stat -c %s "$dir/ng2-pipelines.bin" 2>/dev/null || echo 12) - 12) / 96 )) pipelines"
}

# Chapter 1: New Game to the first fight, fighting (AGENTS.md's route).
story="30000:start,34000:a,37000:a,40000:a,58000:a,63000:a,80000:a,262000:a,280000:a"
run chapter1 400 "$story$(fight 285000 395000)" &

# Mission Mode: title, MISSIONS (two down), the mission's place in the 4x3 grid, then A
# until it starts (its stage loads behind the costume choice), then fighting.
for m in $missions; do
  while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do sleep 5; done
  i=$((m - 1)) s="30000:start,34000:down,35000:down,36000:a" t=39000
  for ((r = 0; r < (i % 12) / 4; r++)); do s="$s,$t:down"; t=$((t + 700)); done
  for ((c = 0; c < i % 4; c++)); do s="$s,$t:right"; t=$((t + 700)); done
  for ((t = t + 1000; t < 112000; t += 2000)); do s="$s,$t:a"; done
  run "mission$(printf %02d "$m")" 230 "$s$(fight 115000 225000)" &
done
wait

python3 -I "$root/tools/merge_pipelines.py" "$out/ng2-pipelines-seed.bin" "$out"/*/ng2-pipelines.bin
