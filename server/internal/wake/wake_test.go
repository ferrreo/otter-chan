package wake

import "testing"

func TestMatches(t *testing.T) {
	phrases := []string{"tarquin", "hey tarquin"}
	for _, s := range []string{"Tarquin", "hey tarquin what's the time", "tarkin, are you there", "Okay Tarquin."} {
		if ok, sc := Matches(s, phrases, 72); !ok {
			t.Errorf("%q should wake (score %d)", s, sc)
		}
	}
	for _, s := range []string{"what time is it", "turn the lights on", "", "in", "a", "the", "ok", "win", "tar", "quin"} {
		if ok, sc := Matches(s, phrases, 72); ok {
			t.Errorf("%q should not wake (score %d)", s, sc)
		}
	}
}

func TestNormalize(t *testing.T) {
	if got := Normalize("Tarkin, what are the bots up to? tarkwin!", "Tarquin"); got != "Tarquin, what are the bots up to? Tarquin!" {
		t.Fatalf("got %q", got)
	}
}
