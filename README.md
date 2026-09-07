# Otter-chan / Tarquin

Custom firmware and server for the **M5Stack StackChan** (CoreS3, ESP32-S3) that turns it into
*Tarquin*: a Victorian butler with a dry wit, a British male voice, a camera that follows you, and a
bridge to your Grok bots.

```
 StackChan (firmware/)  ── WiFi/WebSocket ──►  Tarquin server (server/, Go)  ──►  Muse Spark 1.3 (Meta Model API)
   mic → VAD → PCM                                ├─ parakeet-server (STT, ggml)   ◄──  Grok bots (REST, bearer)
   speaker ◄ TTS PCM                              └─ otter-vox (Audio8 TTS, ggml)
   camera → motion + face assist
   servos, LEDs, face on screen
```

No Python at runtime. Speech runs on CPU via ggml (parakeet.cpp for STT, otter-vox for TTS).

## Features

- **Side button**: click = open mic until you stop talking; click again = close mic; double-click = hard
  mute (wake word off); hold = sleep / wake. The bottom button is the hardware reset line and cannot be
  intercepted by software (it reboots), so sleep lives on the side button hold and on `/api/sleep`.
- **Wake word "tarquin"** (hands-free): on-device VAD ships short clips, server transcribes + fuzzy-matches.
- **Butler**: Muse Spark 1.3 (contributor tier by default) with tools: message bots, read bot status,
  take and look at a photo (multimodal), timers, notes/memory, gestures, lights, volume, sleep, tracking.
- **TTS**: piper by default (native, ~25× real time on CPU, British male voices `en_GB-alan-medium` /
  `en_GB-northern_english_male-medium`), or `OTTER_TTS_ENGINE=vox` for the Audio8 zero-shot "tarquin" clone
  (needs a strong GPU to be quick). Sentence streaming starts speech while the model is still writing.
- **Screen**: animated robot face (blinks, gaze follows you, mouth syncs to speech, expressions from the
  LLM) plus live cards for each Grok bot's activity (coding cursor, browsing globe, thinking dots…).
- **Tracking**: camera motion centroid on-device + server face detection (pure Go, pigo) → head follows you;
  returns home when you leave.
- **Fun**: head-touch tap = happy nod, swipe = volume, hold = hearts; shake it = "I am not a cocktail";
  screen tap = wink, hold = IP/battery; idle fidgets; LED patterns per state; dance gesture; timers spoken aloud.
- **Bots API**: bots post status (drives the screen), speak to you through Tarquin, and pull an inbox of
  things you asked Tarquin to pass on. See [docs/bot-api.md](docs/bot-api.md).
- **Fully wireless**: WiFi + battery; captive-portal setup on first boot (or JSON over USB serial).

## Repo layout

| path | what |
|------|------|
| `firmware/` | PlatformIO (pioarduino, Arduino core 3.x) firmware for CoreS3 + StackChan-BSP |
| `server/` | Go server: WebSocket device link, STT/LLM/TTS pipeline, bot REST API |
| `docker-compose.yml`, `server/Dockerfile` | Coolify deployment (tarquin + parakeet-server) |
| `deploy/voices/` | extra otter-vox voices (`tarquin.codes` + `tarquin.txt`) |
| `tools/voice/` | one-off voice encoder (Python, offline) |
| `docs/` | [protocol](docs/protocol.md), [bot API](docs/bot-api.md), [voice](docs/voice.md) |

## Deploy the server (Coolify)

1. New resource → Docker Compose → this repo. Coolify reads `docker-compose.yml`.
2. Environment: `OTTER_TOKENS=device:<tok>,grok-alpha:<tok>,admin:<tok>` and `META_MODEL_API_KEY=<key>`
   (the Muse Code plan key from `~/.config/muse/auth.json` → `providers.meta.api_key` works). The compose
   file passes `/dev/dri` for Vulkan TTS by default; set `RENDER_GID`/`VIDEO_GID` to the host's group ids
   (`getent group render video`). CPU-only hosts: `OTTER_VOX_ARGS=--cpu --cpu-threads 8`.
3. Set the `tarquin` service domain to `https://bot.ferreo.dev:8480`. Coolify's proxy terminates TLS and
   the robot connects to `wss://bot.ferreo.dev/ws/device` (its default URL). The firmware pins
   Let's Encrypt's ISRG Root X1, so plain `ws://` is only used for LAN testing.
4. Give each Grok bot its token plus the brief at the end of `docs/bot-api.md`.

First boot of the `stt` container downloads `tdt_ctc-110m` (~255 MB) into a volume. The `tarquin`
image includes otter-vox and the Audio8 weights from the PikaOS PPA (Debian sid base for glibc 2.43).

Local run: `cp .env.example .env`, fill it, `docker compose up --build`.

## Flash the robot

```bash
cd firmware
cp secrets.ini.example secrets.ini   # optional compile-time WiFi/server defaults
pio run -t upload                    # /dev/ttyACM1 by default; ~/.local/bin/pio if installed via uv
pio device monitor
```

Without `secrets.ini` the robot boots into setup: join WiFi `OtterChan-XXXX`, open `http://192.168.4.1/`,
enter WiFi, server URL (default `wss://bot.ferreo.dev/ws/device`; `ws://host:8480/ws/device` for LAN), and the device token. Or send
`{"ssid":"…","pass":"…","url":"wss://…/ws/device","token":"…"}` as one line on the USB serial console.
Hold the side button while powering on to re-enter setup. `status` / `reset` on serial also work.

Factory firmware can be restored any time with M5Burner.

## Notes

- piper is instant. The Audio8 clone (`OTTER_TTS_ENGINE=vox`) is 2–3× slower than real time on CPU and
  needs a proper GPU (an Intel UHD 630 is slower than the CPU); a first Vulkan run compiles shaders for ~1 min.
- Muse Spark always reasons: `OTTER_LLM_REASONING=minimal` keeps replies snappy (~2 s, ~120 hidden tokens; `low` roughly doubles that).
- otter-vox needs the headless `--serve --no-play -o -` patch (in this repo's sibling otter-vox tree);
  without it the server falls back to spawning otter-vox per sentence, which reloads the model each time.
