package audio

import (
	"math"
	"testing"
)

func TestADPCMRoundTrip(t *testing.T) {
	n := 2048
	pcm := make([]int16, n)
	for i := range pcm {
		pcm[i] = int16(8000*math.Sin(float64(i)*0.05) + 3000*math.Sin(float64(i)*0.31))
	}
	enc := EncodeADPCM(pcm)
	if len(enc) != 4+n/2 {
		t.Fatalf("block size %d", len(enc))
	}
	dec := DecodeADPCM(enc, n)
	if len(dec) != n {
		t.Fatalf("decoded %d samples", len(dec))
	}
	var sig, noise float64
	for i := range pcm {
		d := float64(pcm[i]) - float64(dec[i])
		sig += float64(pcm[i]) * float64(pcm[i])
		noise += d * d
	}
	snr := 10 * math.Log10(sig/noise)
	if snr < 20 {
		t.Fatalf("SNR too low: %.1f dB", snr)
	}
	t.Logf("SNR %.1f dB", snr)
}
