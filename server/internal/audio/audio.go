// Package audio: minimal WAV framing and resampling for 16-bit PCM.
package audio

import (
	"encoding/binary"
	"errors"
	"io"
	"math"
)

// WrapWAV builds a PCM16 mono WAV file around raw little-endian samples.
func WrapWAV(pcm []byte, rate int) []byte {
	out := make([]byte, 44+len(pcm))
	copy(out[0:], "RIFF")
	binary.LittleEndian.PutUint32(out[4:], uint32(36+len(pcm)))
	copy(out[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(out[16:], 16)
	binary.LittleEndian.PutUint16(out[20:], 1)
	binary.LittleEndian.PutUint16(out[22:], 1)
	binary.LittleEndian.PutUint32(out[24:], uint32(rate))
	binary.LittleEndian.PutUint32(out[28:], uint32(rate*2))
	binary.LittleEndian.PutUint16(out[32:], 2)
	binary.LittleEndian.PutUint16(out[34:], 16)
	copy(out[36:], "data")
	binary.LittleEndian.PutUint32(out[40:], uint32(len(pcm)))
	copy(out[44:], pcm)
	return out
}

// WAV holds decoded PCM16 samples.
type WAV struct {
	Rate     int
	Channels int
	Samples  []int16 // interleaved
}

// ReadWAV parses a complete WAV (PCM16 or float32) from r, reading exactly one file.
func ReadWAV(r io.Reader) (*WAV, error) {
	var hdr [12]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return nil, err
	}
	if string(hdr[0:4]) != "RIFF" || string(hdr[8:12]) != "WAVE" {
		return nil, errors.New("not a WAV stream")
	}
	w := &WAV{}
	var format uint16
	var bits uint16
	for {
		var ch [8]byte
		if _, err := io.ReadFull(r, ch[:]); err != nil {
			return nil, err
		}
		id := string(ch[0:4])
		size := binary.LittleEndian.Uint32(ch[4:8])
		switch id {
		case "fmt ":
			buf := make([]byte, size)
			if _, err := io.ReadFull(r, buf); err != nil {
				return nil, err
			}
			format = binary.LittleEndian.Uint16(buf[0:])
			w.Channels = int(binary.LittleEndian.Uint16(buf[2:]))
			w.Rate = int(binary.LittleEndian.Uint32(buf[4:]))
			bits = binary.LittleEndian.Uint16(buf[14:])
		case "data":
			buf := make([]byte, size)
			if _, err := io.ReadFull(r, buf); err != nil {
				return nil, err
			}
			switch {
			case format == 1 && bits == 16:
				w.Samples = make([]int16, len(buf)/2)
				for i := range w.Samples {
					w.Samples[i] = int16(binary.LittleEndian.Uint16(buf[2*i:]))
				}
			case format == 3 && bits == 32:
				w.Samples = make([]int16, len(buf)/4)
				for i := range w.Samples {
					f := math.Float32frombits(binary.LittleEndian.Uint32(buf[4*i:]))
					w.Samples[i] = int16(math.Max(-32768, math.Min(32767, float64(f)*32767)))
				}
			default:
				return nil, errors.New("unsupported WAV encoding")
			}
			if size%2 == 1 {
				io.ReadFull(r, make([]byte, 1))
			}
			return w, nil
		default:
			if _, err := io.CopyN(io.Discard, r, int64(size+size%2)); err != nil {
				return nil, err
			}
		}
	}
}

// Mono averages channels.
func (w *WAV) Mono() []int16 {
	if w.Channels <= 1 {
		return w.Samples
	}
	n := len(w.Samples) / w.Channels
	out := make([]int16, n)
	for i := 0; i < n; i++ {
		var acc int
		for c := 0; c < w.Channels; c++ {
			acc += int(w.Samples[i*w.Channels+c])
		}
		out[i] = int16(acc / w.Channels)
	}
	return out
}

// Resample converts mono samples from -> to Hz with a windowed-sinc low-pass (good enough for speech).
func Resample(in []int16, from, to int) []int16 {
	if from == to || len(in) == 0 {
		return in
	}
	ratio := float64(from) / float64(to)
	n := int(float64(len(in)) / ratio)
	out := make([]int16, n)
	const taps = 16
	cutoff := 1.0
	if to < from {
		cutoff = float64(to) / float64(from)
	}
	for i := 0; i < n; i++ {
		center := float64(i) * ratio
		c0 := int(center)
		var acc, wsum float64
		for k := c0 - taps; k <= c0+taps; k++ {
			if k < 0 || k >= len(in) {
				continue
			}
			x := (float64(k) - center) * cutoff
			var s float64
			if x == 0 {
				s = 1
			} else {
				s = math.Sin(math.Pi*x) / (math.Pi * x)
			}
			// Hann window
			wv := 0.5 + 0.5*math.Cos(math.Pi*(float64(k)-center)/float64(taps+1))
			wgt := s * wv * cutoff
			acc += float64(in[k]) * wgt
			wsum += wgt
		}
		if wsum != 0 {
			acc /= wsum
		}
		out[i] = int16(math.Max(-32768, math.Min(32767, acc)))
	}
	return out
}

// Normalize scales peak to target (0..1), max gain 8x.
func Normalize(s []int16, target float64) {
	var peak int
	for _, v := range s {
		if a := int(v); a > peak {
			peak = a
		} else if -a > peak {
			peak = -a
		}
	}
	if peak < 1 {
		return
	}
	gain := math.Min(8, target*32767/float64(peak))
	for i, v := range s {
		s[i] = int16(math.Max(-32768, math.Min(32767, float64(v)*gain)))
	}
}

// Bytes converts samples to little-endian PCM16 bytes.
func Bytes(s []int16) []byte {
	out := make([]byte, len(s)*2)
	for i, v := range s {
		binary.LittleEndian.PutUint16(out[2*i:], uint16(v))
	}
	return out
}
