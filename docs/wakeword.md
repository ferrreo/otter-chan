# Training the "tarquin" wake word

The robot detects the wake word on-device with a [microWakeWord](https://github.com/kahrendt/microWakeWord)
streaming model (Apache-2.0) running on tflite-micro. Training is a one-off, offline Python job; the
firmware only ever sees the resulting `.tflite`, embedded as `firmware/src/mww_model.h`.

## Setup (once)

```bash
uv venv --python 3.11 ~/.cache/otter-chan-bench/mww-venv
uv pip install --python ~/.cache/otter-chan-bench/mww-venv/bin/python 'tensorflow[and-cuda]' \
  'git+https://github.com/kahrendt/microWakeWord' torch torchaudio piper-phonemize-cross==1.2.1 datasets scipy pyarrow \
  'git+https://github.com/whatsnowplaying/audio-metadata@d4ebb238e6a401bb1a5aaaac60c9e2b3cb30929f'
W=~/.cache/otter-chan-bench/mww; mkdir -p $W && cd $W
git clone https://github.com/rhasspy/piper-sample-generator && uv pip install --python ../mww-venv/bin/python -e piper-sample-generator
curl -L -o piper-sample-generator/models/en_US-libritts_r-medium.pt \
  https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt
```

Negative feature sets (speech, dinner party, no-speech; ~30 GB unpacked) come from
`huggingface.co/datasets/kahrendt/microwakeword`, background noise from one AudioSet parquet shard,
reverb from the MIT impulse responses. `tools/wakeword/train.py` downloads what the stages need.

## Real recordings (recommended)

- `$W/real_positive/*.wav`: you saying "Tarquin" / "Hey Tarquin", trimmed, 16 kHz mono.
- `$W/real_negative/*.wav`: minutes of normal speech without the word (used as ambient validation and negatives).

## Train and embed

```bash
~/.cache/otter-chan-bench/mww-venv/bin/python tools/wakeword/train.py --work $W --steps 20000
python tools/wakeword/embed_model.py $W/trained_models/tarquin/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite \
  deploy/wakeword/tarquin.json firmware/src/mww_model.h
cd firmware && pio run -t upload
```

`deploy/wakeword/tarquin.json` holds the manifest (probability cutoff, sliding window, arena size);
start from the values in `docs/wakeword.md` and lower `probability_cutoff` if it misses you, raise it
if it false-triggers. The device logs `mww: max prob last 30 s` at debug level to help tune.
