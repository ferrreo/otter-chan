# Otter-chan firmware (M5Stack StackChan / CoreS3)

Build with PlatformIO + [pioarduino](https://github.com/pioarduino/platform-espressif32) (Arduino core 3.x, IDF 5).

```bash
pio run              # build
pio run -t upload    # flash over USB (native USB CDC, /dev/ttyACM1)
pio device monitor   # 115200 baud
```

## Modules

| file | role |
|------|------|
| `main.cpp` | state machine (standby / listening / thinking / speaking / muted / sleep), buttons, timeouts |
| `audio_io.cpp` | mic capture + energy VAD, speaker ring buffer, half-duplex switching (CoreS3 codec) |
| `net_link.cpp` | WebSocket client, tx queue, tagged binary frames |
| `face.cpp` | avatar renderer (PSRAM canvas, 30 fps), status bar, captions, bot cards |
| `tracker.cpp` | GC0308 camera, motion centroid, server face assist, servo follow, scripted moves |
| `behaviors.cpp` | LEDs, head-touch panel, IMU shake, idle fidgets |
| `provisioning.cpp` | captive portal + serial JSON config |
| `settings.cpp` | NVS settings |
| `config.h` | pins, thresholds, tunables (tracking gains/signs, VAD, timeouts) |

## Hardware map (official M5Stack StackChan kit)

- CoreS3: ESP32-S3, 320×240 LCD, GC0308 camera (DVP, SCCB on internal I2C), ES7210 mics, AW88298 speaker, BMI270 IMU
- Body: two SCS0009 serial servos on UART1 (TX 6 / RX 7), 12 WS2812 LEDs + servo power via PY32 IO expander, Si12T 3-zone head touch, INA226 battery monitor
- Side button = AXP2101 power key (`M5.BtnPWR`); bottom button = RST (hardware reset)

Tuning: if the head turns away from you instead of towards you, flip `TRK_YAW_SIGN` / `TRK_PITCH_SIGN`
in `config.h`. `TRK_GAIN`, `TRK_DEADBAND` and `CAM_HFOV_DEG` control how eagerly it follows.
