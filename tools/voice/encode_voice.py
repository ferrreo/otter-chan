#!/usr/bin/env python3
"""One-off: encode a reference clip into otter-vox voice files (NAME.codes + NAME.txt).

The runtime (otter-vox / ggml) never runs Python; this only produces the 10 x frames u16 codec-code
matrix that Audio8 uses as the zero-shot voice prompt. Needs torch + transformers and the
Audio8/Audio8-TTS-Preview-0.6b checkpoint (see docs/voice.md).
"""
import argparse
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
import torch
from transformers import AutoModel


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="local dir of Audio8/Audio8-TTS-Preview-0.6b")
    ap.add_argument("--audio", required=True, type=Path, help="reference WAV/FLAC, 0.5-30 s, one speaker")
    ap.add_argument("--text", help="exact transcript (or --text-file)")
    ap.add_argument("--text-file", type=Path)
    ap.add_argument("--name", required=True, help="voice name, e.g. tarquin")
    ap.add_argument("--out", type=Path, default=Path("deploy/voices"))
    args = ap.parse_args()

    text = args.text or (args.text_file.read_text().strip() if args.text_file else "")
    if not text:
        sys.exit("--text or --text-file required (exact transcript of the clip)")

    audio, rate = sf.read(args.audio, dtype="float32", always_2d=True)
    mono = torch.from_numpy(audio.mean(axis=1))
    model = AutoModel.from_pretrained(args.model, trust_remote_code=True, dtype=torch.float32).eval()
    target = int(model.config.codec_sample_rate)
    if rate != target:
        from torchaudio.functional import resample
        mono = resample(mono, rate, target)
    dur = mono.numel() / target
    if not 0.5 <= dur <= 30:
        sys.exit(f"clip is {dur:.1f}s; use 0.5-30 s")
    values = mono.unsqueeze(0).unsqueeze(0)  # [B=1, C=1, N]
    lengths = torch.tensor([mono.numel()])
    with torch.inference_mode():
        codes, code_lengths = model.encode_audio(values, lengths)
    codes = codes[0, :, : int(code_lengths[0])].cpu().numpy()  # [10, T]
    if codes.shape[0] != 10 or codes.min() < 0 or codes.max() > 65535:
        sys.exit(f"unexpected codes shape/range {codes.shape}")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / f"{args.name}.codes").write_bytes(codes.astype("<u2").tobytes(order="C"))
    (args.out / f"{args.name}.txt").write_text(text.strip() + "\n")
    print(f"wrote {args.out}/{args.name}.codes ({codes.shape[1]} frames, {dur:.1f}s) and {args.name}.txt")


if __name__ == "__main__":
    main()
