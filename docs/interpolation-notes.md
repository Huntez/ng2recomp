# Frame interpolation: notes from testing (2026-11-07)

On the user's PC: RX 6800, Windows, 4K at 180 Hz, NG2_EXCLUSIVE=1.

## Settings

- `NG2_INTERP_FRAMES` sets the pictures between game frames: 1 is 2x (120 fps), 2 is 3x (180 fps,
  the default), 3 is 4x.
- 2x on a 180 Hz monitor doesn't divide evenly: 2 pictures for 3 refreshes, so one stays two
  refreshes and the next one. Motion is uneven, mostly in camera turns. For 2x, set the monitor to
  120 Hz; at 180 Hz use 3x.
- `NG2_INTERP_SLOTS` (default 3 with interpolation): frame slots. Three let the game, the build of
  the pictures between and the GPU overlap, which fixed the fps drops in crowds (25-28 ms frames
  with two).
- `NG2_INTERP_IMAGES` (default 8): swapchain images. With 4, black tearing-like bars appeared, so
  keep 8.
- `NG2_INTERP_WORKERS` (default up to 4): threads for mixing bone palettes.

## Latency (NG2_LATENCY_LOG=1, pad read to present)

- The game alone, in the fire-arrow fight: 33.5 ms. It presents two of its frames after reading
  the pad.
- With interpolation: 37-39 ms. Before the bones were spread over threads it was 37-42 ms. The
  extra time is the render thread waiting for the build of the pictures between. What remains is
  matching (1-2 ms) and constants (about 1 ms).
- The full new position also reaches the screen about two refreshes (11 ms at 180 Hz) after the
  first picture between. Any frame generation has this.
- Input Latency Low doesn't work on AMD's Windows driver: it needs VK_KHR_present_wait, which that
  driver doesn't have. The display-clock latency log needs it too.

## Open

- The death or continue screen: the counter shows 60, but the screen looks badly slowed down. Its
  log shows 300 of 600 frames as partial frames: the scene is drawn every other frame, and in
  between only a few passes go over the last picture. A partial frame gets no pictures between, and
  the next full frame interpolates from the last full one, two frames back. Each full frame's
  pictures go out at once (exclusive full screen), so the screen moves for a refresh or two, then
  holds for about four.
- More generally: with interpolation on, anything that only reaches 60 (cuts, partial frames,
  screens with nothing to interpolate) feels much worse than 180 next to it.
- Rare limb "tube" flickers.
- Exclusive full screen sometimes isn't taken at start, or is lost later ("full screen exclusive:
  no / still no" in the log).
