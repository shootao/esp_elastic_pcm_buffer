# Changelog

-------------------

## v0.5.3

### Changed

- Pipeline consumer task no longer polls with a delay when no PCM frames are available. It now blocks on task notifications and wakes on `elastic_pcm_buffer_push()`, `elastic_pcm_buffer_session_end()`, and destroy/shutdown.
- `elastic_pcm_buffer_pipeline_cfg_t::consumer_idle_ms` is now deprecated and ignored. It is kept only for backward source compatibility.
- Example applications no longer set `consumer_idle_ms` in pipeline configuration.

## v0.5.2

### Changed

- Component manifest homepage and links: `url`, `documentation`, and `issues` now target [github.com/shootao/esp_elastic_pcm_buffer](https://github.com/shootao/esp_elastic_pcm_buffer) (README and issue tracker).

## v0.5.1

### Features

- Optional event notifications: `elastic_pcm_buffer_set_event_handler()`, `elastic_pcm_buffer_event_to_string()`, and events `CONSUMER_STARVED`, `CONSUMER_RECOVERED`, `PUSH_OVERFLOW`, `POP_UNDERFLOW` (payload includes a status snapshot).
- English documentation: root `README.md` (translation of `README_CN.md`).

### Examples

- `examples/basic_pipeline`: log lines include boot-time timestamps `[t=… ms]` and output callback logs `arrival_ms` (frame enqueue time) for latency inspection.

## v0.5.0

### Features

- Initial version of esp_elastic_pcm_buffer
