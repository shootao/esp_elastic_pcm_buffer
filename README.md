# esp_elastic_pcm_buffer

[中文版](README_CN.md)

`esp_elastic_pcm_buffer` is an elastic buffering component for real-time PCM audio streams.

Its goal is not simply to “store a few more packets,” but to keep playback smoother and more predictable when network delivery timing is irregular:

- Buffer first when data arrives unevenly
- Slow consumption slightly when the buffer runs low
- Speed consumption slightly when the buffer runs high
- At session end, either drop the tail immediately or drain remaining frames

Typical use cases:

- WebSocket voice chat
- Cloud TTS streaming PCM
- LAN or Internet real-time PCM playback
- Light jitter without a full RTP / jitter-buffer stack

## Features

`esp_elastic_pcm_buffer` provides:

- Fixed-capacity PCM frame buffer
- Four watermarks: `start` / `low` / `target` / `high`
- Automatic prefill
- Underflow / overflow statistics
- Recommended playback speed from current fill level
- Integration with Sonic from `esp_audio_effects` for mild time-scale adjustment
- Internal consumer task + output callback (pipeline mode)
- Session-style control with `session_begin` / `session_end`
- Optional event notifications via `elastic_pcm_buffer_set_event_handler()` (consumer starve/recover, push overflow, pop underflow); see API comments in `include/elastic_pcm_buffer.h`

## Problem being solved

Suppose playback ideally consumes one PCM frame every `60 ms`, but the network does not deliver one frame every `60 ms`:

- Sometimes a frame arrives after `30 ms`
- Sometimes after `90 ms`
- Occasionally several slow arrivals in a row

Playing immediately on receive tends to cause:

- Speedups and gaps
- Sudden buffer underruns
- Or ever-growing latency as the buffer fills

The approach of `esp_elastic_pcm_buffer` is:

1. Store data in a buffer
2. Use watermarks to tell whether the level is low, healthy, or high
3. Choose normal, slightly slower, or slightly faster playback accordingly

So it behaves like a **PCM elastic buffer layer with watermark policy and mild speed adjustment**.

## Usage modes

Two conceptual layers:

### 1. Low-level manual mode

Basic buffering only:

- Application calls `elastic_pcm_buffer_push()` to enqueue
- Application decides when to call `elastic_pcm_buffer_pop()`

Suited when:

- You already have a playback clock
- You want full control over consumption timing
- You only need the buffer and watermark logic, not the internal task

### 2. High-level pipeline mode (recommended)

- Application keeps calling `elastic_pcm_buffer_push()`
- Component starts an internal consumer task
- Component applies watermark logic, Sonic processing, and output callback

Typical API sequence:

```c
elastic_pcm_buffer_create()
elastic_pcm_buffer_set_pipeline()
elastic_pcm_buffer_start()
elastic_pcm_buffer_session_begin()
elastic_pcm_buffer_push()
```

Suited for:

- WebSocket voice chat
- Streaming TTS
- Receive-and-play PCM
- “Feed data only” without implementing your own consumption clock

## Watermarks

The core configuration uses four thresholds:

```c
size_t start_watermark;
size_t low_watermark;
size_t target_watermark;
size_t high_watermark;
```

Meaning:

- **`start_watermark`**  
  Minimum number of frames before consumption is allowed to start in earnest.

- **`low_watermark`**  
  Low line: below this, the buffer is considered lean.

- **`target_watermark`**  
  Desired steady-state level the controller tries to stay near.

- **`high_watermark`**  
  High line: above this, backlog (end-to-end delay) is growing.

### Watermark diagram

```text
0 ---- low ---- target ---- high ---- capacity
      ^          ^           ^
      |          |           |
   low zone   ideal zone   high zone

start_watermark:
only controls “when playback may start”
```

## Public state enum

Reported states include:

- `ELASTIC_PCM_BUFFER_STATE_PREFILLING`
- `ELASTIC_PCM_BUFFER_STATE_NORMAL`
- `ELASTIC_PCM_BUFFER_STATE_LOW_WATER`
- `ELASTIC_PCM_BUFFER_STATE_HIGH_WATER`
- `ELASTIC_PCM_BUFFER_STATE_UNDERFLOW`

### `PREFILLING`

- Still accumulating frames
- Data may be arriving but playback should not fully start yet

Typical triggers:

- Right after start
- After flush
- After the buffer was drained and prefill resumes

### `NORMAL`

- Fill level is in a healthy band
- Playback can follow a normal pace

### `LOW_WATER`

- Buffer is lean
- Higher risk of underrun

Tendency:

- Sonic speed moves slightly **below** `1.0x`
- Slow consumption a little to let the buffer recover

### `HIGH_WATER`

- Buffer is high
- End-to-end delay is increasing

Tendency:

- Sonic speed moves slightly **above** `1.0x`
- Speed up playback slightly to reduce backlog

### `UNDERFLOW`

- No consumable data
- Buffer is empty

Effect:

- Return to prefill behavior until data accumulates again

## Speed compensation

This component is not meant for extreme time-stretching; only **mild** speed adjustment.

Rough policy:

- Below target level → recommended speed below `1.0x`
- Near target → recommended speed near `1.0x`
- Above target → recommended speed above `1.0x`

So:

- Low: play a bit slower to protect the buffer
- High: play a bit faster to reduce latency

Sonic performs the actual processing.

## Session control

The design fits **session-based** use (e.g. chat turns), not a single endless stream only.

Example:

- End of a turn: `elastic_pcm_buffer_session_end()`
- Start of next turn: `elastic_pcm_buffer_session_begin()`

This avoids carrying leftover audio from the previous turn into the next.

## Stop modes

`elastic_pcm_buffer_session_end()` supports two behaviors.

### `ELASTIC_PCM_BUFFER_STOP_MODE_DISCARD`

- Drop all remaining frames in the buffer immediately
- Sonic does not process tail audio for that stop

Use when:

- The utterance is over
- Tail audio should not play
- Next content matters more

### `ELASTIC_PCM_BUFFER_STOP_MODE_DRAIN_DIRECT`

- Output remaining queued frames directly
- No watermark-based pacing and no Sonic adjustment on that tail

Use when:

- The session ends but tail audio should still be heard

## Examples

See the [examples](examples) directory.

Main examples:

- **[examples/compare_playback](examples/compare_playback)**  
  Comparison between `direct` and `elastic` paths. Targets Korvo-2, reads `/sdcard/jb.pcm` from SD card, includes jitter simulation and console control.

- **[examples/basic_pipeline](examples/basic_pipeline)**  
  Minimal pipeline example: basic integration order and callback output; not focused on board-specific audio routing.

If you are new to this component:

- For listening tests, tuning, and direct vs elastic comparison, start with [examples/compare_playback](examples/compare_playback).
- For the shortest API walkthrough, start with [examples/basic_pipeline](examples/basic_pipeline).
