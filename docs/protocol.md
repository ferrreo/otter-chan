# Device ↔ server protocol

One WebSocket per robot: `GET /ws/device` with `Authorization: Bearer <device token>` and
`X-Otter-Name: <robot name>`. Text frames are JSON objects with a `type`. Binary frames start with a
one-byte tag.

## Binary tags

| tag  | direction | payload |
|------|-----------|---------|
| 0x01 | both      | 16 kHz s16le mono PCM. Device→server: mic frames while listening. Server→device: TTS audio. |
| 0x02 | device→server | one wake-word candidate clip (≤ 2.5 s PCM) |
| 0x03 | device→server | JPEG photo (reply to `photo_request`) |
| 0x04 | device→server | small JPEG for face-detect assist (~every 700 ms while tracking) |

## Device → server (JSON)

| type | fields | meaning |
|------|--------|---------|
| `hello` | name, fw, rate, mode, wake_word, tracking | sent on connect |
| `state` | state | mode changed: standby, listening, thinking, speaking, muted, sleep |
| `listen_start` | | mic opened; PCM frames follow |
| `listen_end` | reason: silence, max | utterance complete; run the pipeline |
| `listen_cancel` | | mic closed without speech |
| `cancel` | | user pressed the button while thinking/speaking |
| `telemetry` | battery_v, battery_ma, battery_pct, charging, rssi, yaw, pitch, target, camera, heap, psram, uptime_s | every 10 s |
| `event` | name: head_tap, head_hold, shake | physical interaction |
| `photo_failed` | | camera unavailable |

## Server → device (JSON)

| type | fields | meaning |
|------|--------|---------|
| `hello_ack` | name, wake_phrases, volume? | |
| `wake` | | wake phrase heard: open the mic |
| `thinking` | | STT/LLM running |
| `transcript` | text, final | show what was heard |
| `say_start` | text, expression, followup | TTS begins; PCM (0x01) follows |
| `caption` | text, ms | update the caption line (ms=0 keeps it until the next) |
| `say_end` | followup | all audio sent; device returns to standby (or re-opens mic if followup) |
| `expression` | name, ms | neutral, happy, sad, surprised, thinking, sleepy, angry, love, confused, wink, error |
| `bots` | bots: [{name, activity, detail, emoji, age_s}] | drives the bot cards at the bottom of the screen |
| `notify` | text, from | chime + LED pulse + caption |
| `look` | x, y, speed | -1..1 normalised head position |
| `gesture` | name: nod, shake, dance, home | |
| `led` | mode: off, solid, pulse, rainbow; r, g, b, ms | |
| `face` | found, x, y, w | face-detect assist result (0..1 image coords) |
| `photo_request` | quality | device answers with tag 0x03 |
| `listen` | | open the mic as if the button was pressed |
| `cancel` | | stop speaking / abort |
| `sleep` | | go to sleep |
| `config` | volume (0-255), tracking, wake_word, brightness | persisted in NVS |
| `reboot` | | |

## Pipeline

```
button / wake ─► listen_start ─► PCM… ─► listen_end (VAD silence 0.9 s)
   server: parakeet STT ─► transcript ─► Muse Spark (tools) ─► per sentence: otter-vox TTS ─► PCM 0x01
   device: plays as chunks arrive (ring ~3 s), mouth animates from RMS ─► say_end ─► standby / follow-up listen
```

Wake word: in standby the device runs an energy VAD; on a speech burst it ships a ≤2.5 s clip (0x02).
The server transcribes it and fuzzy-matches "tarquin" (plus common mishearings). A hit sends `wake`.
