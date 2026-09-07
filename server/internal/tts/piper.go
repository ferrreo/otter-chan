package tts

import (
	"bytes"
	"context"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"otter-chan/server/internal/audio"
)

// Synth is what the rest of the server needs from a TTS engine.
type Synth interface {
	Synthesize(ctx context.Context, text string) ([]byte, error) // 16 kHz s16le mono
	Warm(ctx context.Context) error
	Healthy() bool
	Name() string
}

// Piper runs rhasspy/piper (native C++, ONNX) per sentence: ~20x faster than real time on a CPU.
// One process per request keeps things simple; model load is a few hundred milliseconds.
type Piper struct {
	Bin      string
	Model    string // path to voice .onnx (the .onnx.json beside it gives the sample rate)
	Rate     int
	CacheDir string
	Speed    float64 // length_scale: >1 slower, <1 faster

	modelRate int
	mu        sync.Mutex
}

func NewPiper(bin, model string, rate int, cacheDir string, speed float64) *Piper {
	os.MkdirAll(cacheDir, 0o755)
	p := &Piper{Bin: bin, Model: model, Rate: rate, CacheDir: cacheDir, Speed: speed, modelRate: 22050}
	if raw, err := os.ReadFile(model + ".json"); err == nil {
		var cfg struct {
			Audio struct {
				SampleRate int `json:"sample_rate"`
			} `json:"audio"`
		}
		if json.Unmarshal(raw, &cfg) == nil && cfg.Audio.SampleRate > 0 {
			p.modelRate = cfg.Audio.SampleRate
		}
	}
	return p
}

func (p *Piper) Name() string { return "piper:" + strings.TrimSuffix(filepath.Base(p.Model), ".onnx") }

func (p *Piper) Healthy() bool {
	_, err := exec.LookPath(p.Bin)
	if err != nil {
		return false
	}
	_, err = os.Stat(p.Model)
	return err == nil
}

func (p *Piper) Warm(ctx context.Context) error {
	t0 := time.Now()
	_, err := p.Synthesize(ctx, "Good day.")
	log.Printf("tts: piper warm-up done in %.1fs (err=%v)", time.Since(t0).Seconds(), err)
	return err
}

func (p *Piper) Synthesize(ctx context.Context, text string) ([]byte, error) {
	text = strings.TrimSpace(strings.ReplaceAll(text, "\n", " "))
	if text == "" {
		return nil, nil
	}
	key := sha1.Sum([]byte(p.Name() + "|" + text))
	cachePath := filepath.Join(p.CacheDir, hex.EncodeToString(key[:])+".pcm")
	if b, err := os.ReadFile(cachePath); err == nil {
		return b, nil
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	ctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	args := []string{"--model", p.Model, "--output_raw", "--sentence_silence", "0.15"}
	if p.Speed > 0 && p.Speed != 1 {
		args = append(args, "--length_scale", fmt.Sprintf("%.2f", p.Speed))
	}
	cmd := exec.CommandContext(ctx, p.Bin, args...)
	cmd.Stdin = strings.NewReader(text + "\n")
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("piper: %v: %s", err, strings.TrimSpace(lastLine(errb.String())))
	}
	raw := out.Bytes()
	if len(raw) < 4 {
		return nil, fmt.Errorf("piper produced no audio: %s", strings.TrimSpace(lastLine(errb.String())))
	}
	samples := make([]int16, len(raw)/2)
	for i := range samples {
		samples[i] = int16(uint16(raw[2*i]) | uint16(raw[2*i+1])<<8)
	}
	mono := audio.Resample(samples, p.modelRate, p.Rate)
	audio.Normalize(mono, 0.9)
	pcm := audio.Bytes(mono)
	if len(text) < 120 {
		os.WriteFile(cachePath, pcm, 0o644)
	}
	return pcm, nil
}

func lastLine(s string) string {
	lines := strings.Split(strings.TrimSpace(s), "\n")
	return lines[len(lines)-1]
}
