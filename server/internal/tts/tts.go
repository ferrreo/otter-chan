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

	firstTimeout time.Duration
	skip         int // renders abandoned by a cancelled caller; their WAVs are still coming
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
	// Drain results of renders that a cancelled caller walked away from.
	for e.skip > 0 {
		if _, err := e.readWAV(ctx, e.timeout()); err != nil {
			return nil, err
		}
		e.skip--
	}
	if _, err := io.WriteString(e.stdin, text+"\n"); err != nil {
		e.stop()
		return nil, err
	}
	w, err := e.readWAV(ctx, e.timeout())
	if err != nil {
		return nil, err
	}
	e.serveOK = true
	return w, nil
}

// readWAV waits for one WAV from the serve process. A caller cancel leaves the process alone
// (the render finishes in the background and is drained later); a timeout or read error restarts it.
func (e *Engine) readWAV(ctx context.Context, timeout time.Duration) (*audio.WAV, error) {
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
		return r.w, nil
	case <-time.After(timeout):
		e.stop()
		return nil, errors.New("tts timeout")
	case <-ctx.Done():
		e.skip++
		go func() { <-ch }() // let the reader goroutine finish; result is drained on the next call
		return nil, ctx.Err()
	}
}

func (e *Engine) timeout() time.Duration {
	if e.firstTimeout > 0 {
		return e.firstTimeout
	}
	return 180 * time.Second
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

// Warm starts the resident engine (model load, Vulkan shader compile) and renders one phrase
// bypassing the cache, so the first real reply doesn't pay for it.
func (e *Engine) Warm(ctx context.Context) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.checkVoice()
	t0 := time.Now()
	e.firstTimeout = 10 * time.Minute // model load + Vulkan shader compile can take a while
	_, err := e.synth(ctx, "Good day.")
	e.firstTimeout = 0
	log.Printf("tts: warm-up done in %.1fs (err=%v)", time.Since(t0).Seconds(), err)
	return err
}

// checkVoice falls back to an embedded clone only if the configured voice exists neither as an
// embedded otter-vox voice nor as a voices/NAME.codes file next to the model.
func (e *Engine) checkVoice() {
	if e.Voice == FallbackVoice {
		return
	}
	out, _ := exec.Command(e.Bin, "--list-voices").Output()
	for _, line := range strings.Split(string(out), "\n") {
		if strings.TrimSpace(line) == e.Voice {
			return
		}
	}
	dirs := []string{"/usr/share/otter-shell/models/vox/audio8"}
	for i, a := range e.Args {
		if a == "--model-dir" && i+1 < len(e.Args) {
			dirs = append([]string{e.Args[i+1]}, dirs...)
		}
	}
	for _, d := range dirs {
		if _, err := os.Stat(filepath.Join(d, "voices", e.Voice+".codes")); err == nil {
			return
		}
	}
	log.Printf("tts: voice %q not found (embedded or voices/%s.codes); using %q", e.Voice, e.Voice, FallbackVoice)
	e.Voice = FallbackVoice
}

func (e *Engine) Healthy() bool {
	_, err := exec.LookPath(e.Bin)
	return err == nil
}
