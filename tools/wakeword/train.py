#!/usr/bin/env python3
"""Train a "tarquin" wake-word model with microWakeWord (offline, one-off; the robot runs the
resulting tflite model natively, no Python).

Stages (each cached in --work):
  samples   synthetic positives with piper-sample-generator (several spellings, many speakers)
  rirs      MIT impulse responses (reverb augmentation)
  noise     16 kHz background clips from an AudioSet shard
  features  augmented spectrograms (RaggedMmap) for train/validation/test
  train     microWakeWord mixednet, exports the quantized streaming tflite

Real recordings help: put WAVs of the word in --work/real_positive and speech without the word
in --work/real_negative before the features stage; they join validation/testing (and training).

Usage:
  python tools/wakeword/train.py --work ~/.cache/otter-chan-bench/mww --steps 20000
Needs the venv from docs/wakeword.md (tensorflow, microwakeword, piper-sample-generator).
"""
from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys
from pathlib import Path

SPELLINGS = [  # phonetic variants so the model hears the word many ways
    ("tarquin", 1200),
    ("tar-quin", 400),
    ("tarkwin", 400),
    ("hey tarquin", 900),
    ("hey tar-quin", 300),
    ("ok tarquin", 400),
    ("okay tarquin", 300),
]
# words that sound similar but must NOT trigger
HARD_NEGATIVES = [("tarmac", 150), ("darwin", 150), ("turkey", 100), ("talking", 150), ("harlequin", 100), ("target", 100), ("hey there", 150)]


def run(cmd: list[str], cwd: Path | None = None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def stage_samples(work: Path, gen_model: Path, py: str):
    out = work / "generated_samples"
    if out.exists() and len(list(out.glob("*.wav"))) > 1000:
        return
    out.mkdir(exist_ok=True)
    for text, n in SPELLINGS:
        d = work / "gen" / text.replace(" ", "_")
        if d.exists() and len(list(d.glob("*.wav"))) >= n:
            pass
        else:
            run([py, "-m", "piper_sample_generator", text, "--model", str(gen_model), "--max-samples", str(n), "--batch-size", "50",
                 "--max-speakers", "300", "--length-scales", "0.9", "1.0", "1.15", "--output-dir", str(d)], cwd=work / "piper-sample-generator")
        for i, w in enumerate(sorted(d.glob("*.wav"))):
            link = out / f"{text.replace(' ', '_')}_{i:05d}.wav"
            if not link.exists():
                os.symlink(w.resolve(), link)
    neg = work / "generated_negatives"
    neg.mkdir(exist_ok=True)
    for text, n in HARD_NEGATIVES:
        d = work / "gen_neg" / text.replace(" ", "_")
        if not (d.exists() and len(list(d.glob("*.wav"))) >= n):
            run([py, "-m", "piper_sample_generator", text, "--model", str(gen_model), "--max-samples", str(n), "--batch-size", "50",
                 "--max-speakers", "300", "--output-dir", str(d)], cwd=work / "piper-sample-generator")
        for i, w in enumerate(sorted(d.glob("*.wav"))):
            link = neg / f"{text.replace(' ', '_')}_{i:05d}.wav"
            if not link.exists():
                os.symlink(w.resolve(), link)


def stage_rirs(work: Path):
    out = work / "data" / "mit_rirs"
    if out.exists() and any(out.glob("*.wav")):
        return
    import datasets, numpy as np, scipy.io.wavfile
    out.mkdir(parents=True, exist_ok=True)
    ds = datasets.load_dataset("davidscripka/MIT_environmental_impulse_responses", split="train", streaming=True)
    for row in ds:
        name = row["audio"]["path"].split("/")[-1]
        scipy.io.wavfile.write(out / name, 16000, (row["audio"]["array"] * 32767).astype(np.int16))


def stage_noise(work: Path):
    """16 kHz mono WAVs from an AudioSet parquet shard (agkphysics/AudioSet, data/bal_train/00.parquet)."""
    out = work / "data" / "audioset_16k"
    if out.exists() and len(list(out.glob("*.wav"))) > 500:
        return
    import io, numpy as np, scipy.io.wavfile, soundfile as sf, pyarrow.parquet as pq
    from scipy.signal import resample_poly
    from math import gcd
    out.mkdir(parents=True, exist_ok=True)
    shards = sorted((work / "data" / "audioset").glob("*.parquet"))
    i = 0
    for shard in shards:
        pf = pq.ParquetFile(shard)
        for batch in pf.iter_batches(batch_size=64, columns=["audio"]):
            for item in batch.column("audio").to_pylist():
                raw = item.get("bytes") if isinstance(item, dict) else None
                if not raw:
                    continue
                try:
                    a, r = sf.read(io.BytesIO(raw), dtype="float32", always_2d=True)
                except Exception:
                    continue
                a = a.mean(axis=1)
                if r != 16000:
                    g = gcd(r, 16000)
                    a = resample_poly(a, 16000 // g, r // g)
                scipy.io.wavfile.write(out / f"{i:05d}.wav", 16000, (np.clip(a, -1, 1) * 32767).astype(np.int16))
                i += 1
    print(f"audioset clips: {i}")


def stage_features(work: Path):
    from microwakeword.audio.augmentation import Augmentation
    from microwakeword.audio.clips import Clips
    from microwakeword.audio.spectrograms import SpectrogramGeneration
    from mmap_ninja.ragged import RaggedMmap

    bg = [str(work / "data" / "audioset_16k")]
    aug = Augmentation(
        augmentation_duration_s=3.2,
        augmentation_probabilities={"SevenBandParametricEQ": 0.1, "TanhDistortion": 0.1, "PitchShift": 0.1, "BandStopFilter": 0.1,
                                    "AddColorNoise": 0.1, "AddBackgroundNoise": 0.75, "Gain": 1.0, "RIR": 0.5},
        impulse_paths=[str(work / "data" / "mit_rirs")], background_paths=bg,
        background_min_snr_db=-5, background_max_snr_db=10, min_jitter_s=0.195, max_jitter_s=0.205,
    )

    def build(name: str, clip_dir: Path, splits: dict[str, tuple[str, int, int]]):
        out_root = work / "features" / name
        if (out_root / "training").exists() or (out_root / "validation").exists():
            return
        clips = Clips(input_directory=str(clip_dir), file_pattern="*.wav", max_clip_duration_s=None, remove_silence=False,
                      random_split_seed=10, split_count=0.1)
        for split, (split_name, repetition, slide) in splits.items():
            out_dir = out_root / split
            out_dir.mkdir(parents=True, exist_ok=True)
            spec = SpectrogramGeneration(clips=clips, augmenter=aug, slide_frames=slide, step_ms=10)
            RaggedMmap.from_generator(out_dir=str(out_dir / "wakeword_mmap"),
                                      sample_generator=spec.spectrogram_generator(split=split_name, repeat=repetition),
                                      batch_size=100, verbose=True)

    std = {"training": ("train", 2, 10), "validation": ("validation", 1, 10), "testing": ("test", 1, 1)}
    build("positive", work / "generated_samples", std)
    build("hard_negative", work / "generated_negatives", std)
    real_pos = work / "real_positive"
    if real_pos.exists() and any(real_pos.glob("*.wav")):
        build("real_positive", real_pos, std)
    real_neg = work / "real_negative"
    if real_neg.exists() and any(real_neg.glob("*.wav")):
        build("real_negative", real_neg, {"validation_ambient": ("validation", 1, 1), "testing_ambient": ("test", 1, 1), "training": ("train", 1, 10)})


def stage_train(work: Path, py: str, steps: int):
    import yaml
    neg = work / "data" / "negative_datasets"
    feats = [
        {"features_dir": str(work / "features" / "positive"), "sampling_weight": 2.0, "penalty_weight": 1.0, "truth": True, "truncation_strategy": "truncate_start", "type": "mmap"},
        {"features_dir": str(work / "features" / "hard_negative"), "sampling_weight": 3.0, "penalty_weight": 2.0, "truth": False, "truncation_strategy": "truncate_start", "type": "mmap"},
        {"features_dir": str(neg / "speech"), "sampling_weight": 10.0, "penalty_weight": 1.0, "truth": False, "truncation_strategy": "random", "type": "mmap"},
        {"features_dir": str(neg / "dinner_party"), "sampling_weight": 10.0, "penalty_weight": 1.0, "truth": False, "truncation_strategy": "random", "type": "mmap"},
        {"features_dir": str(neg / "no_speech"), "sampling_weight": 5.0, "penalty_weight": 1.0, "truth": False, "truncation_strategy": "random", "type": "mmap"},
        {"features_dir": str(neg / "dinner_party_eval"), "sampling_weight": 0.0, "penalty_weight": 1.0, "truth": False, "truncation_strategy": "split", "type": "mmap"},
    ]
    if (work / "features" / "real_positive").exists():
        feats.append({"features_dir": str(work / "features" / "real_positive"), "sampling_weight": 1.0, "penalty_weight": 1.5, "truth": True, "truncation_strategy": "truncate_start", "type": "mmap"})
    if (work / "features" / "real_negative").exists():
        feats.append({"features_dir": str(work / "features" / "real_negative"), "sampling_weight": 2.0, "penalty_weight": 2.0, "truth": False, "truncation_strategy": "split", "type": "mmap"})
    cfg = {
        "window_step_ms": 10, "train_dir": str(work / "trained_models" / "tarquin"), "features": feats,
        "training_steps": [steps], "positive_class_weight": [1], "negative_class_weight": [20], "learning_rates": [0.001],
        "batch_size": 128, "time_mask_max_size": [0], "time_mask_count": [0], "freq_mask_max_size": [0], "freq_mask_count": [0],
        "eval_step_interval": 500, "clip_duration_ms": 1500, "target_minimization": 0.9, "minimization_metric": None,
        "maximization_metric": "average_viable_recall",
    }
    (work / "training_parameters.yaml").write_text(yaml.dump(cfg))
    run([py, "-m", "microwakeword.model_train_eval", "--training_config", str(work / "training_parameters.yaml"), "--train", "1",
         "--restore_checkpoint", "1", "--test_tf_nonstreaming", "0", "--test_tflite_nonstreaming", "0", "--test_tflite_nonstreaming_quantized", "0",
         "--test_tflite_streaming", "0", "--test_tflite_streaming_quantized", "1", "--use_weights", "best_weights",
         "mixednet", "--pointwise_filters", "64,64,64,64", "--repeat_in_block", "1, 1, 1, 1", "--mixconv_kernel_sizes", "[5], [7,11], [9,15], [23]",
         "--residual_connection", "0,0,0,0", "--first_conv_filters", "32", "--first_conv_kernel_size", "5", "--stride", "3"], cwd=work)
    print("model:", work / "trained_models" / "tarquin" / "tflite_stream_state_internal_quant" / "stream_state_internal_quant.tflite")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--stages", default="samples,rirs,noise,features,train")
    ap.add_argument("--steps", type=int, default=20000)
    ap.add_argument("--gen-model", type=Path, default=None, help="piper-sample-generator .pt (default: work/piper-sample-generator/models/en_US-libritts_r-medium.pt)")
    a = ap.parse_args()
    work = a.work.expanduser().resolve()
    py = sys.executable
    gen_model = a.gen_model or work / "piper-sample-generator" / "models" / "en_US-libritts_r-medium.pt"
    for st in a.stages.split(","):
        print(f"=== stage {st}", flush=True)
        {"samples": lambda: stage_samples(work, gen_model, py), "rirs": lambda: stage_rirs(work), "noise": lambda: stage_noise(work),
         "features": lambda: stage_features(work), "train": lambda: stage_train(work, py, a.steps)}[st]()


if __name__ == "__main__":
    main()
