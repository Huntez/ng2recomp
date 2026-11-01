#!/usr/bin/env bash
# Silent test run: nested headless gamescope, no audio device, no input
# devices opened, scratch HOME/XDG dirs and config. Nothing reaches the
# desktop, the speakers or the user's settings.
#
# usage: tools/run-headless.sh <seconds> <out_dir> [extra ng2 args...]
# Writes <out_dir>/ng2.log.
#   NG2_SHOTS="10 30 60"  gamescope screenshots at those seconds -> <out_dir>/shot-<s>.png
#   NG2_WRAP="gdb -batch -x cmds.gdb --args"  put in front of the binary
#   NG2_TEST_WAYLAND=1  run on gamescope's nested Wayland socket instead of X11
#   NG2_TEST_AUDIO=disk  real audio output, written to <out_dir>/audio.raw
#     (SDL's disk driver) instead of the null audio device
#   NG2_TEST_AUDIO=pipewire NG2_PW_RUNTIME=<dir>  audio in real time through
#     a private PipeWire at that runtime dir (one null sink, no sound cards)
set -euo pipefail

secs=${1:?seconds}
out=$(realpath -m "${2:?out dir}")
shift 2

root=$(cd "$(dirname "$0")/.." && pwd)
bin=${NG2_BIN:-$root/out/build/linux-amd64-release/ng2}
mkdir -p "$out/xdg/data" "$out/xdg/config" "$out/xdg/cache" "$out/user" "$out/home"
# NG2_SAVE_FROM=<user_data_root>: start from a copy of those saves (the
# originals are never opened).
if [[ -n ${NG2_SAVE_FROM:-} ]]; then cp -a "$NG2_SAVE_FROM/." "$out/user/"; fi

# Settings from the user's shell (REX_* cvar variables) must not leak in.
unset $(compgen -e | grep '^REX') 2>/dev/null || true

# Runs inside gamescope: start the game, take the requested screenshots.
# The game gets its own time limit: one that hangs would otherwise outlive
# gamescope, orphaned under its reaper.
inner='
shots=$1 out=$2 secs=$3; shift 3
timeout --kill-after=5 "$secs" "$@" &
pid=$!
t=0
for s in $shots; do
  sleep $((s - t)); t=$s
  kill -0 $pid 2>/dev/null || break
  gamescopectl screenshot "$out/shot-$s.png" >/dev/null 2>&1 || true
done
wait $pid
'

video=(SDL_VIDEODRIVER=x11 SDL_VIDEO_DRIVER=x11)
gamescope_flags=()
if [ "${NG2_TEST_WAYLAND:-0}" = 1 ]; then
  # --expose-wayland points WAYLAND_DISPLAY at gamescope's own socket.
  video=(SDL_VIDEODRIVER=wayland SDL_VIDEO_DRIVER=wayland)
  set -- --video_driver=wayland "$@"  # ReXGlue picks the SDL driver from its cvar
  gamescope_flags=(--expose-wayland)
fi

audio=(NG2_AUDIO=0 SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy)
if [ "${NG2_TEST_AUDIO:-}" = disk ]; then
  audio=(NG2_AUDIO=1 SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE="$out/audio.raw")
elif [ "${NG2_TEST_AUDIO:-}" = pipewire ]; then
  # A real-time audio device on a private PipeWire (NG2_PW_RUNTIME: its
  # runtime dir, one null sink, no sound cards), never the user's.
  audio=(NG2_AUDIO=1 SDL_AUDIO_DRIVER=pipewire PIPEWIRE_RUNTIME_DIR="${NG2_PW_RUNTIME:?NG2_PW_RUNTIME}"
         PULSE_SERVER=disabled)
fi

exec env -u WAYLAND_DISPLAY -u DISPLAY HOME="$out/home" NG2_CONFIG="$out/ng2.toml" \
  "${audio[@]}" \
  "${video[@]}" \
  SDL_GAMECONTROLLER_IGNORE_DEVICES=0x2dc8/0x310a \
  SDL_JOYSTICK_IGNORE_DEVICES=0x2dc8/0x310a \
  XDG_DATA_HOME="$out/xdg/data" XDG_CONFIG_HOME="$out/xdg/config" XDG_CACHE_HOME="$out/xdg/cache" \
  timeout --kill-after=10 "$secs" \
  gamescope --backend headless "${gamescope_flags[@]}" -W 1280 -H 720 -w 1280 -h 720 -- \
  bash -c "$inner" inner "${NG2_SHOTS:-}" "$out" "$secs" \
  ${NG2_WRAP:-} "$bin" --game_data_root="$root/game_files" --user_data_root="$out/user" \
  --input_backend=none --log_file="$out/ng2.log" "$@"
