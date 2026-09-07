// Package tts drives otter-vox (Audio8 TTS on ggml) as a resident subprocess.
//
// Preferred: `otter-vox --serve --no-play -o - --voice V` — one text line in, one complete WAV out.
// Fallback (older otter-vox without headless serve): spawn `otter-vox --no-play -o - TEXT` per request.
package tts

import (
	"bufio"
	"context"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"otter-chan/server/internal/audio"
)

type Engine struct {
	Bin      string
	Voice    string
	Args     []string
	Rate     int // output rate for the robot
	CacheDir string

	mu       sync.Mutex
	cmd      *exec.Cmd
	stdin    io.WriteCloser
	stdout   *bufio.Reader
	serveOK  bool
	serveBad bool
}

func New(bin, voice string, args []string, rate, cacheDir string) *Engine {
	os.MkdirAll(cacheDir, 0o755)
	return &Engine{Bin: bin, Voice: voice, Args: args, Rate: atoi(rate), CacheDir: cacheDir}
}

func atoi(s string) int {
	n := 0
	fmt.Sscanf(s, "%d", &n)
	return n
}

func (e *Engine) baseArgs() []string {
	args := []string{"--no-play", "-o", "-", "--voice", e.Voice}
	return append(args, e.Args...)
}

// start launches the resident serve process. Returns error if unsupported.
func (e *Engine) start() error {
	if e.serveBad {
		return errors.New("serve mode unavailable")
	}
	args := append([]string{"--serve"}, e.baseArgs()...)
	cmd := exec.Command(e.Bin, args...)
	cmd.Stderr = os.Stderr
	in, err := cmd.StdinPipe()
	if err != nil {
		return err
	}
	out, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return err
	}
	e.cmd, e.stdin, e.stdout = cmd, in, bufio.NewReaderSize(out, 1<<20)
	log.Printf("tts: otter-vox serve started (pid %d, voice %s)", cmd.Process.Pid, e.Voice)
	return nil
}

func (e *Engine) stop() {
	if e.cmd != nil {
		e.stdin.Close()
		e.cmd.Process.Kill()
		e.cmd.Wait()
	}
	e.cmd, e.stdin, e.stdout = nil, nil, nil
}

// Synthesize returns 16 kHz s16le PCM for text.
func (e *Engine) Synthesize(ctx context.Context, text string) ([]byte, error) {
	text = strings.TrimSpace(strings.ReplaceAll(text, "\n", " "))
	if text == "" {
		return nil, nil
	}
	key := sha1.Sum([]byte(e.Voice + "|" + text))
	cachePath := filepath.Join(e.CacheDir, hex.EncodeToString(key[:])+".pcm")
	if b, err := os.ReadFile(cachePath); err == nil {
		return b, nil
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	w, err := e.synth(ctx, text)
	if err != nil && e.Voice != FallbackVoice {
		// An otter-vox without runtime-loadable voices rejects the clone with VoiceNotFound.
		log.Printf("tts: voice %q failed (%v); falling back to %q", e.Voice, err, FallbackVoice)
		e.Voice = FallbackVoice
		e.stop()
		e.serveBad = false
		w, err = e.synth(ctx, text)
	}
	if err != nil {
		return nil, err
	}
	mono := audio.Resample(w.Mono(), w.Rate, e.Rate)
	audio.Normalize(mono, 0.9)
	pcm := audio.Bytes(mono)
	if len(text) < 120 {
		os.WriteFile(cachePath, pcm, 0o644)
	}
	return pcm, nil
}

// FallbackVoice is an embedded otter-vox clone that every build has.
const FallbackVoice = "audio8-en-calm"

func (e *Engine) synth(ctx context.Context, text string) (*audio.WAV, error) {
	w, err := e.viaServe(ctx, text)
	if err == nil {
		return w, nil
	}
	if !e.serveBad {
		log.Printf("tts: serve path failed (%v); trying per-call spawn", err)
	}
	return e.viaSpawn(ctx, text)
}

func (e *Engine) viaServe(ctx context.Context, text string) (*audio.WAV, error) {
	if e.cmd == nil {
		if err := e.start(); err != nil {
			e.serveBad = true
			return nil, err
		}
	}
	if _, err := io.WriteString(e.stdin, text+"\n"); err != nil {
		e.stop()
		return nil, err
	}
	type res struct {
		w   *audio.WAV
		err error
	}
	ch := make(chan res, 1)
	go func() {
		w, err := audio.ReadWAV(e.stdout)
		ch <- res{w, err}
	}()
	select {
	case r := <-ch:
		if r.err != nil {
			e.stop()
			// an old binary rejects --serve with --no-play and exits immediately
			if !e.serveOK {
				e.serveBad = true
			}
			return nil, r.err
		}
		e.serveOK = true
		return r.w, nil
	case <-time.After(120 * time.Second):
		e.stop()
		return nil, errors.New("tts timeout")
	case <-ctx.Done():
		e.stop()
		return nil, ctx.Err()
	}
}

func (e *Engine) viaSpawn(ctx context.Context, text string) (*audio.WAV, error) {
	ctx, cancel := context.WithTimeout(ctx, 180*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, e.Bin, append(e.baseArgs(), text)...)
	cmd.Stderr = os.Stderr
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	w, rerr := audio.ReadWAV(bufio.NewReader(out))
	io.Copy(io.Discard, out)
	if err := cmd.Wait(); err != nil && rerr != nil {
		return nil, fmt.Errorf("otter-vox: %v", err)
	}
	return w, rerr
}

func (e *Engine) Healthy() bool {
	_, err := exec.LookPath(e.Bin)
	return err == nil
}
