# SDK needs for `world-radio`

This file collects the platform features `world-radio` is missing. It is written to be forwarded to the BoBo firmware/SDK team; the app itself is complete and ships without them, using the workarounds described below.

## Need 1: streaming HTTP for live radio

### Why

The firmware exposes buffered requests only:

- `bobo_http()` downloads the whole response into an app buffer (`out_cap - 1` bytes) within a ~20 s timeout, HTTPS only, and blocks the app task while it runs (queued system events are delivered after it returns);
- `bobo_audio_*` accepts PCM only, and the host already resamples to the speaker.

The app works around this with HLS: it fetches one ~7 s ADTS AAC segment at a time (`https://ls.qingting.fm/live/<id>.m3u8?bitrate=64` for mainland China, public broadcaster m3u8s elsewhere), demuxes MPEG-TS when needed, decodes with Helix AAC-LC, and keeps ~5 s of PCM queued while the next segment downloads. That is enough for 243 live stations, but:

- playback starts after the first full segment downloads, typically 1–3 s, and each segment boundary risks a gap on slow Wi-Fi;
- every fetch blocks the app task for its duration, so Back and the clock can only react between segments;
- ICY/HTTP infinite MP3 streams (Icecast; what most ESP32 radio projects play) remain unusable, as does any stream without an HLS wrapper.

### Proposed API (API level 7)

```c
/* Streaming HTTPS/HTTP response. An app importing these declares "api": 7. */

/* Opens a request and waits for the response head (follows redirects).
 * Returns a handle > 0, or -1 on failure. *status (optional) receives the
 * final HTTP status once known. body/body_len apply to POST. */
int bobo_http_stream_open(const char* method, const char* url,
                          const bobo_header_t* headers, size_t header_count,
                          const char* body, size_t body_len, int* status);

/* Reads up to size response bytes. Returns >0 bytes read, 0 when timeout_ms
 * passed with no data ready, -1 on error. timeout_ms 0 polls. The call does
 * bounded socket work, like bobo_auth_inbox_poll, so the app task can keep
 * handling BOBO_EV_EXIT between reads. */
int bobo_http_stream_read(int handle, void* buf, size_t size, uint32_t timeout_ms);

/* True after the response body ended (all bytes returned) or the stream died. */
bool bobo_http_stream_eof(int handle);

/* Case-insensitive lookup of a response header, e.g. "Content-Type" or
 * "icy-metaint"; returns the value length, or -1. */
int bobo_http_stream_header(int handle, const char* name, char* out, size_t out_cap);

/* Idempotent. App exit closes any open stream as well. */
void bobo_http_stream_close(int handle);
```

Requirements:

- TLS plus redirects plus chunked transfer, same trust rules as `bobo_http`. Live radio is often plain HTTP (Icecast); allow `http://` URLs explicitly for public streams (no silent HTTPS→HTTP downgrade). If plain HTTP stays forbidden, streaming is still useful for HTTPS podcast CDNs, but should be stated.
- One stream per app at a time is enough. All calls belong to the app task, outside a drawing frame; memory stays bounded (a few KB of socket buffer).
- Read timeout without data should not kill the stream. The app can then pause, process events, and resume reading.
- Expose at least `Content-Type` and `icy-metaint`. The first tells MP3 from AAC; the second lets the app skip ICY metadata blocks.
- New capability bit, e.g. `BOBO_CAP_HTTP_STREAM` (1u << 9), with manifest gating like the other API levels.

### What this unlocks

- `bobo_http_stream_open("GET", stream_url, ...)`, then read 4 KB chunks with a ~50 ms timeout inside the player loop: parse ADTS/TS frames, feed AAC into the existing Helix path, and write PCM. Pause stops reading; close on exit.
- ICY/HTTP live stations from the existing catalog, plus gapless HLS segment fetching without a blocking 20 s budget.
- Start in 1–2 s, no per-segment download gap, and Back reacting within a frame.

### Not required

- HLS/DASH demuxing or AAC decoding. The app already does both.
- Background playback. Foreground live playback with `bobo_keep_awake(true)` is sufficient.
- An MP3 decoder. The app is live-only and ships Helix AAC; more AAC streams are enough.

### Acceptance sketch

With Wi-Fi connected, open an HTTPS AAC stream URL (HLS or Icecast AAC), read chunks with a 50 ms timeout while the UI remains responsive, hear audio within 2 s, pause/resume, press Back, and return from `app_main` within 3 s with the socket and audio closed.

## Need 2: read the system UI language

### Why

The About page offers 跟随系统 / 中文 / English, and the manifest text is written in Chinese. "跟随系统" should start the app in whatever language BoBo OS is set to. API 6 provides no way to read that:

- `include/bobo_app.h` has no locale/language getter and no `BOBO_CAP_LANGUAGE` capability bit;
- `bobo_state_*` is per-app private storage (`state.json` inside the app package), not a window into system settings, so known keys like `system_lang`, `ui_lang`, `locale`, `language` never exist.

The app therefore probes those state keys best-effort and falls back to Chinese when nothing is found. The About page says so on screen. Manual switching works and persists, but the default is not actually "system".

### Proposed API (API level 7)

```c
/* BCP-47-ish tag of the language currently selected in BoBo OS settings,
 * e.g. "zh-CN" or "en-US". Returns the tag length, 0 if the system has no
 * language preference, or -1 if unsupported. */
int bobo_system_language(char* out, size_t out_cap);

/* Optional: true when the state key lives in system settings rather than the
 * app package, i.e. bobo_state_get("ui_lang", ...) becomes meaningful. */
bool bobo_state_system(const char* key);
```

Either one is enough for this app: read once at startup, match the primary subtag (`zh*` → Chinese, `en*` → English), and keep the manual override on top. No new capability bit is needed if the function simply returns -1 on older firmware.