package tts

import (
	"context"
	"os"
	"testing"
)

// Runs only when OTTER_PIPER_TEST_BIN / OTTER_PIPER_TEST_MODEL point at a local piper install.
func TestPiperSynthesize(t *testing.T) {
	bin, model := os.Getenv("OTTER_PIPER_TEST_BIN"), os.Getenv("OTTER_PIPER_TEST_MODEL")
	if bin == "" || model == "" {
		t.Skip("set OTTER_PIPER_TEST_BIN and OTTER_PIPER_TEST_MODEL")
	}
	p := NewPiper(bin, model, 16000, t.TempDir(), 1.0)
	pcm, err := p.Synthesize(context.Background(), "Good evening, sir.")
	if err != nil {
		t.Fatal(err)
	}
	secs := float64(len(pcm)) / 32000
	if secs < 0.5 || secs > 5 {
		t.Fatalf("unexpected duration %.2fs", secs)
	}
	t.Logf("%.2fs of audio, model rate %d", secs, p.modelRate)
}
