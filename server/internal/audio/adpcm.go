package audio

// IMA ADPCM (4 bits/sample). Each block is self-contained: 4-byte header (int16 predictor LE,
// uint8 step index, uint8 reserved) followed by nibble pairs, so any block can be decoded alone.

var imaIndexTable = [16]int{-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8}

var imaStepTable = [89]int{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
	157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
	1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
	12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
}

func clampInt(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

// EncodeADPCM encodes samples into one block.
func EncodeADPCM(pcm []int16) []byte {
	out := make([]byte, 4+(len(pcm)+1)/2)
	if len(pcm) == 0 {
		return out
	}
	pred := int(pcm[0])
	index := 0
	out[0] = byte(pred)
	out[1] = byte(pred >> 8)
	out[2] = byte(index)
	for i, s := range pcm {
		step := imaStepTable[index]
		diff := int(s) - pred
		var nibble int
		if diff < 0 {
			nibble = 8
			diff = -diff
		}
		delta := 0
		if diff >= step {
			nibble |= 4
			diff -= step
			delta += step
		}
		if diff >= step>>1 {
			nibble |= 2
			diff -= step >> 1
			delta += step >> 1
		}
		if diff >= step>>2 {
			nibble |= 1
			delta += step >> 2
		}
		delta += step >> 3
		if nibble&8 != 0 {
			pred -= delta
		} else {
			pred += delta
		}
		pred = clampInt(pred, -32768, 32767)
		index = clampInt(index+imaIndexTable[nibble], 0, 88)
		if i%2 == 0 {
			out[4+i/2] = byte(nibble)
		} else {
			out[4+i/2] |= byte(nibble << 4)
		}
	}
	return out
}

// DecodeADPCM decodes one block. n limits the sample count (0 = all nibbles).
func DecodeADPCM(data []byte, n int) []int16 {
	if len(data) < 4 {
		return nil
	}
	pred := int(int16(uint16(data[0]) | uint16(data[1])<<8))
	index := clampInt(int(data[2]), 0, 88)
	total := (len(data) - 4) * 2
	if n > 0 && n < total {
		total = n
	}
	out := make([]int16, total)
	for i := 0; i < total; i++ {
		b := data[4+i/2]
		var nibble int
		if i%2 == 0 {
			nibble = int(b & 0x0f)
		} else {
			nibble = int(b >> 4)
		}
		step := imaStepTable[index]
		delta := step >> 3
		if nibble&4 != 0 {
			delta += step
		}
		if nibble&2 != 0 {
			delta += step >> 1
		}
		if nibble&1 != 0 {
			delta += step >> 2
		}
		if nibble&8 != 0 {
			pred -= delta
		} else {
			pred += delta
		}
		pred = clampInt(pred, -32768, 32767)
		index = clampInt(index+imaIndexTable[nibble], 0, 88)
		out[i] = int16(pred)
	}
	return out
}

// Samples converts little-endian PCM16 bytes to samples.
func Samples(b []byte) []int16 {
	out := make([]int16, len(b)/2)
	for i := range out {
		out[i] = int16(uint16(b[2*i]) | uint16(b[2*i+1])<<8)
	}
	return out
}
