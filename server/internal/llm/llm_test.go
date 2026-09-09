package llm

import "testing"

func TestSplitAndSentences(t *testing.T) {
	e, body := SplitExpression("[happy] Very good, sir. Tea is served! Anything else?")
	if e != "happy" {
		t.Fatalf("expr %q", e)
	}
	s := Sentences(body)
	if len(s) != 3 || s[0] != "Very good, sir." || s[2] != "Anything else?" {
		t.Fatalf("sentences %#v", s)
	}
	if e, _ := SplitExpression("no tag here."); e != "neutral" {
		t.Fatal("expected neutral")
	}
}

func TestSentenceStreamer(t *testing.T) {
	var got []string
	ss := &sentenceStreamer{expr: "neutral", emit: func(e, s string) { got = append(got, e+"|"+s) }}
	for _, d := range []string{"[", "wink", "] Very", " good, sir", ". Tea", " is served! And", " more"} {
		ss.push(d)
	}
	ss.flush()
	want := []string{"wink|Very good, sir.", "wink|Tea is served!", "wink|And more"}
	if len(got) != len(want) {
		t.Fatalf("got %#v", got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("got %#v want %#v", got, want)
		}
	}
}

func TestSplitTags(t *testing.T) {
	e, g, b := SplitTags("[happy nod] Very good, sir.")
	if e != "happy" || g != "nod" || b != "Very good, sir." {
		t.Fatalf("got %q %q %q", e, g, b)
	}
	e, g, _ = SplitTags("[surprised, excited] A visitor!")
	if e != "surprised" || g != "excited" {
		t.Fatalf("got %q %q", e, g)
	}
}
