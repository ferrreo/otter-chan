// Package wake decides whether a transcript contains the wake phrase.
package wake

import (
	"regexp"
	"strings"
)

var clean = regexp.MustCompile(`[^a-z0-9 ]+`)

// Aliases: common mis-hearings of "tarquin" so a small ASR model still wakes reliably.
var Aliases = []string{"tarquin", "tarkin", "tarkwin", "tar quin", "torquin", "tarquine", "darkwin", "tark win", "tarquinn", "tarquim"}

// Matches returns (hit, score 0..100) for text against phrases and aliases.
func Matches(text string, phrases []string, threshold int) (bool, int) {
	t := strings.TrimSpace(clean.ReplaceAllString(strings.ToLower(text), " "))
	if t == "" {
		return false, 0
	}
	best := 0
	for _, p := range append(append([]string{}, phrases...), Aliases...) {
		if s := partialRatio(p, t); s > best {
			best = s
		}
	}
	return best >= threshold, best
}

// partialRatio ~ rapidfuzz.partial_ratio: best similarity of needle against any window of haystack.
func partialRatio(needle, hay string) int {
	n, h := []rune(needle), []rune(hay)
	if len(n) == 0 || len(h) == 0 {
		return 0
	}
	if len(h) < len(n) {
		n, h = h, n
	}
	best := 0
	for start := 0; start+len(n) <= len(h)+len(n)/2; start++ {
		end := start + len(n)
		if end > len(h) {
			end = len(h)
		}
		win := h[start:end]
		d := levenshtein(n, win)
		score := 100 * (len(n) + len(win) - d) / (len(n) + len(win))
		if score > best {
			best = score
		}
	}
	return best
}

func levenshtein(a, b []rune) int {
	prev := make([]int, len(b)+1)
	cur := make([]int, len(b)+1)
	for j := range prev {
		prev[j] = j
	}
	for i := 1; i <= len(a); i++ {
		cur[0] = i
		for j := 1; j <= len(b); j++ {
			cost := 1
			if a[i-1] == b[j-1] {
				cost = 0
			}
			cur[j] = min(prev[j]+1, cur[j-1]+1, prev[j-1]+cost)
		}
		prev, cur = cur, prev
	}
	return prev[len(b)]
}

var aliasRe = regexp.MustCompile(`(?i)\b(tark?w?in|tarquine|torquin|darkwin|tark win|tar quin|tarquinn|tarquim)\b`)

// Normalize rewrites common mis-hearings of the wake word to its canonical spelling.
func Normalize(text, canonical string) string {
	return aliasRe.ReplaceAllString(text, canonical)
}
