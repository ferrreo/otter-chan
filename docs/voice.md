# Tarquin's voice

Default engine is **piper** (`OTTER_TTS_ENGINE=piper`, voice `OTTER_PIPER_MODEL`, speed `OTTER_PIPER_SPEED`).
The image ships `en_GB-alan-medium` and `en_GB-northern_english_male-medium`; any voice from
[rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices) works if you add it to the Dockerfile.
The rest of this page is about the Audio8 clone used when `OTTER_TTS_ENGINE=vox`.

TTS is [otter-vox](https://github.com/pika-os) (Audio8 TTS 0.6b on ggml, zero-shot voice cloning).
A clone is a reference clip encoded to codec codes plus its exact transcript. otter-vox ships two
embedded clones (`fer`, `audio8-en-calm`); this project adds runtime-loadable clones from
`<model dir>/voices/NAME.codes` + `NAME.txt` (otter-vox ≥ the headless-serve patch).

The shipped `tarquin` voice is cloned from a public-domain LibriVox recording by Peter Yearsley
(*Secret Chambers and Hiding Places*, chapter 2, ~7.7 s) — an English male narrator. Files live in
`deploy/voices/` and are copied into the image.

## Making your own clone

1. Record or pick 5–15 s of clean speech (one speaker, no music), WAV, any rate.
2. Write the exact transcript.
3. Encode (one-off, needs Python + torch; runtime stays native):

```bash
uv venv --python 3.12 .venv && uv pip install --python .venv/bin/python \
  --index-url https://download.pytorch.org/whl/cpu torch torchaudio && \
  uv pip install --python .venv/bin/python 'transformers>=4.57,<5' numpy soundfile safetensors 'huggingface_hub[cli]'
hf download Audio8/Audio8-TTS-Preview-0.6b --local-dir /tmp/audio8-0.6b
.venv/bin/python tools/voice/encode_voice.py --model /tmp/audio8-0.6b \
  --audio my_butler.wav --text "Exact transcript of the clip." --name tarquin --out deploy/voices
```

4. Redeploy (`OTTER_VOX_VOICE=tarquin`). Preview: `curl -H "Authorization: Bearer $TOKEN" "$HOST/api/tts?text=Good+evening,+sir." -o t.wav`.

Locally: put the files in `/usr/share/otter-shell/models/vox/audio8/voices/` (or `--model-dir`) and run
`otter-vox --voice tarquin --play "Good evening, sir."`.
