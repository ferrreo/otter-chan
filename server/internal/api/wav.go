package api

import "otter-chan/server/internal/audio"

func wrapWAV(pcm []byte, rate int) []byte { return audio.WrapWAV(pcm, rate) }
