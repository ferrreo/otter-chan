// Package api exposes the bearer-token REST API used by Grok bots and other clients.
package api

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"

	"otter-chan/server/internal/config"
	"otter-chan/server/internal/device"
	"otter-chan/server/internal/llm"
	"otter-chan/server/internal/store"
	"otter-chan/server/internal/stt"
	"otter-chan/server/internal/tts"
)

type API struct {
	Cfg   *config.Config
	Store *store.Store
	Hub   *device.Hub
	STT   *stt.Client
	TTS   tts.Synth
	LLM   *llm.Client
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]any{"error": msg})
}

func readJSON(r *http.Request, v any) error {
	return json.NewDecoder(io.LimitReader(r.Body, 1<<20)).Decode(v)
}

func (a *API) robot(w http.ResponseWriter) *device.Session {
	s := a.Hub.Current()
	if s == nil {
		fail(w, http.StatusServiceUnavailable, "robot not connected")
	}
	return s
}

func ident(r *http.Request) string { return r.Header.Get("X-Otter-Identity") }

// Routes registers everything under /api on mux (caller wraps with auth middleware).
func (a *API) Routes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/status", a.status)
	mux.HandleFunc("GET /api/bots", a.bots)
	mux.HandleFunc("POST /api/bots/{name}/status", a.setStatus)
	mux.HandleFunc("POST /api/bots/{name}/say", a.botSay)
	mux.HandleFunc("GET /api/bots/{name}/inbox", a.inbox)
	mux.HandleFunc("POST /api/bots/{name}/inbox/ack", a.ack)
	mux.HandleFunc("POST /api/bots/{name}/message", a.botMessage)
	mux.HandleFunc("POST /api/say", a.say)
	mux.HandleFunc("POST /api/ask", a.ask)
	mux.HandleFunc("POST /api/expression", a.expression)
	mux.HandleFunc("POST /api/gesture", a.gesture)
	mux.HandleFunc("POST /api/led", a.led)
	mux.HandleFunc("POST /api/look", a.look)
	mux.HandleFunc("GET /api/photo", a.photo)
	mux.HandleFunc("POST /api/config", a.configure)
	mux.HandleFunc("POST /api/sleep", a.sleep)
	mux.HandleFunc("POST /api/listen", a.listen)
	mux.HandleFunc("GET /api/notes", a.notes)
	mux.HandleFunc("GET /api/history", a.history)
	mux.HandleFunc("GET /api/tts", a.ttsPreview)
}

func (a *API) status(w http.ResponseWriter, r *http.Request) {
	s := a.Hub.Current()
	robot := map[string]any{"connected": s != nil}
	if s != nil {
		robot["name"], robot["state"], robot["telemetry"] = s.Name, s.State(), s.Telemetry.Load()
	}
	ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
	defer cancel()
	writeJSON(w, 200, map[string]any{
		"robot": robot, "bots": a.Store.Bots(0), "timers": a.Store.Timers(), "you": ident(r),
		"stt": a.STT.Healthy(ctx), "tts": a.TTS.Healthy(), "tts_engine": a.TTS.Name(), "llm": a.Cfg.LLMAPIKey != "", "model": a.Cfg.LLMModel,
	})
}

func (a *API) bots(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, map[string]any{"bots": a.Store.Bots(0)})
}

func (a *API) setStatus(w http.ResponseWriter, r *http.Request) {
	var in struct{ Activity, Detail, Emoji string }
	if err := readJSON(r, &in); err != nil {
		fail(w, 400, "bad json")
		return
	}
	if !store.Activities[in.Activity] {
		fail(w, 400, "activity must be one of: idle coding browsing thinking talking writing error offline done waiting")
		return
	}
	b := a.Store.SetStatus(r.PathValue("name"), in.Activity, in.Detail, in.Emoji)
	if s := a.Hub.Current(); s != nil {
		s.PushBots()
	}
	writeJSON(w, 200, b)
}

// botSay: bot -> user, spoken by the robot (queued if busy).
func (a *API) botSay(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Text       string
		Speak      *bool
		Expression string
		Chime      *bool
	}
	if err := readJSON(r, &in); err != nil || strings.TrimSpace(in.Text) == "" {
		fail(w, 400, "text required")
		return
	}
	name := r.PathValue("name")
	a.Store.Touch(name)
	s := a.Hub.Current()
	speak := in.Speak == nil || *in.Speak
	chime := in.Chime == nil || *in.Chime
	if s != nil {
		if speak {
			text := fmt.Sprintf("Message from %s: %s", name, in.Text)
			if in.Expression != "" {
				text = "[" + in.Expression + "] " + text
			}
			s.Announce(text, name, chime)
		} else {
			s.SendJSON(map[string]any{"type": "notify", "text": in.Text, "from": name})
		}
	}
	writeJSON(w, 200, map[string]any{"ok": true, "queued": s != nil})
}

// inbox: messages for this bot; ?wait=N long-polls for up to N seconds.
func (a *API) inbox(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	a.Store.Touch(name)
	wait, _ := strconv.Atoi(r.URL.Query().Get("wait"))
	if wait > 60 {
		wait = 60
	}
	pending := a.Store.Pending(name)
	if len(pending) == 0 && wait > 0 {
		ch := a.Store.Subscribe()
		defer a.Store.Unsubscribe(ch)
		deadline := time.After(time.Duration(wait) * time.Second)
	loop:
		for {
			select {
			case ev := <-ch:
				if ev.Event == "message" && ev.Bot == name {
					pending = a.Store.Pending(name)
					if len(pending) > 0 {
						break loop
					}
				}
			case <-deadline:
				break loop
			case <-r.Context().Done():
				return
			}
		}
	}
	writeJSON(w, 200, map[string]any{"messages": pending})
}

func (a *API) ack(w http.ResponseWriter, r *http.Request) {
	var in struct {
		IDs []string `json:"ids"`
	}
	readJSON(r, &in)
	writeJSON(w, 200, map[string]any{"acked": a.Store.Ack(r.PathValue("name"), in.IDs)})
}

// botMessage: bot -> another bot (to=<bot>) or bot -> user silently (to=user).
func (a *API) botMessage(w http.ResponseWriter, r *http.Request) {
	var in struct{ Text, To string }
	if err := readJSON(r, &in); err != nil || in.Text == "" {
		fail(w, 400, "text required")
		return
	}
	name := r.PathValue("name")
	if in.To == "" || in.To == "user" {
		if s := a.Hub.Current(); s != nil {
			s.SendJSON(map[string]any{"type": "notify", "text": in.Text, "from": name})
		}
		writeJSON(w, 200, map[string]any{"ok": true})
		return
	}
	writeJSON(w, 200, map[string]any{"ok": true, "message": a.Store.SendToBot(in.To, in.Text, name)})
}

func (a *API) say(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Text, Expression string
		Chime            *bool
	}
	if err := readJSON(r, &in); err != nil || strings.TrimSpace(in.Text) == "" {
		fail(w, 400, "text required")
		return
	}
	s := a.robot(w)
	if s == nil {
		return
	}
	text := in.Text
	if in.Expression != "" {
		text = "[" + in.Expression + "] " + text
	}
	s.Announce(text, "", in.Chime != nil && *in.Chime)
	writeJSON(w, 200, map[string]any{"ok": true})
}

// ask: run text through Tarquin's brain; spoken if the robot is idle, else text only.
func (a *API) ask(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Text  string
		Speak *bool
	}
	if err := readJSON(r, &in); err != nil || strings.TrimSpace(in.Text) == "" {
		fail(w, 400, "text required")
		return
	}
	s := a.Hub.Current()
	speak := in.Speak == nil || *in.Speak
	if s != nil && speak && !s.Busy() {
		reply, err := s.RespondTo(r.Context(), in.Text, nil)
		if err != nil {
			fail(w, 502, err.Error())
			return
		}
		writeJSON(w, 200, map[string]any{"reply": reply, "spoken": true})
		return
	}
	var parts []string
	expr := "neutral"
	run := func(ctx context.Context, name string, args map[string]any) (any, []byte) {
		if s != nil {
			return s.ToolRunner()(ctx, name, args)
		}
		return map[string]any{"error": "robot offline; tool unavailable"}, nil
	}
	_, err := a.LLM.Respond(r.Context(), in.Text, nil, run, func(e, sen string) { expr = e; parts = append(parts, sen) })
	if err != nil {
		fail(w, 502, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"reply": strings.Join(parts, " "), "expression": expr, "spoken": false})
}

func (a *API) expression(w http.ResponseWriter, r *http.Request) {
	s := a.robot(w)
	if s == nil {
		return
	}
	ms, _ := strconv.Atoi(r.URL.Query().Get("ms"))
	if ms == 0 {
		ms = 3000
	}
	s.SendJSON(s.J("expression", "name", r.URL.Query().Get("name"), "ms", ms))
	writeJSON(w, 200, map[string]any{"ok": true})
}

func (a *API) gesture(w http.ResponseWriter, r *http.Request) {
	s := a.robot(w)
	if s == nil {
		return
	}
	n := r.URL.Query().Get("name")
	switch n {
	case "nod", "shake", "dance", "bow", "spin", "wiggle", "look_around", "excited", "peek", "home":
	default:
		fail(w, 400, "name must be nod|shake|dance|bow|spin|wiggle|look_around|excited|peek|home")
		return
	}
	s.SendJSON(s.J("gesture", "name", n))
	writeJSON(w, 200, map[string]any{"ok": true})
}

func qint(r *http.Request, k string) int { n, _ := strconv.Atoi(r.URL.Query().Get(k)); return n }
func qfloat(r *http.Request, k string) float64 {
	f, _ := strconv.ParseFloat(r.URL.Query().Get(k), 64)
	return f
}

func (a *API) led(w http.ResponseWriter, r *http.Request) {
	s := a.robot(w)
	if s == nil {
		return
	}
	mode := r.URL.Query().Get("mode")
	if mode == "" {
		mode = "solid"
	}
	s.SendJSON(s.J("led", "mode", mode, "r", qint(r, "r"), "g", qint(r, "g"), "b", qint(r, "b"), "ms", qint(r, "ms")))
	writeJSON(w, 200, map[string]any{"ok": true})
}

func (a *API) look(w http.ResponseWriter, r *http.Request) {
	s := a.robot(w)
	if s == nil {
		return
	}
	speed := qint(r, "speed")
	if speed == 0 {
		speed = 500
	}
	s.SendJSON(s.J("look", "x", qfloat(r, "x"), "y", qfloat(r, "y"), "speed", speed))
	writeJSON(w, 200, map[string]any{"ok": true})
}

func (a *API) photo(w http.ResponseWriter, r *http.Request) {
	s := a.robot(w)
	if s == nil {
		return
	}
	q := qint(r, "quality")
	if q == 0 {
		q = 20
	}
	jpg, err := s.RequestPhoto(r.Context(), q)
	if err != nil {
		fail(w, 504, err.Error())
		return
	}
	w.Header().Set("Content-Type", "image/jpeg")
	w.Write(jpg)
}

func (a *API) configure(w http.ResponseWriter, r *http.Request) {
	s := a.robot(w)
	if s == nil {
		return
	}
	var in map[string]any
	if err := readJSON(r, &in); err != nil {
		fail(w, 400, "bad json")
		return
	}
	msg := map[string]any{"type": "config"}
	for _, k := range []string{"volume", "tracking", "wake_word", "brightness"} {
		if v, ok := in[k]; ok {
			msg[k] = v
		}
	}
	s.SendJSON(msg)
	writeJSON(w, 200, map[string]any{"ok": true, "applied": msg})
}

func (a *API) sleep(w http.ResponseWriter, r *http.Request) {
	if s := a.robot(w); s != nil {
		s.SendJSON(s.J("sleep"))
		writeJSON(w, 200, map[string]any{"ok": true})
	}
}

func (a *API) listen(w http.ResponseWriter, r *http.Request) {
	if s := a.robot(w); s != nil {
		s.SendJSON(s.J("listen"))
		writeJSON(w, 200, map[string]any{"ok": true})
	}
}

func (a *API) notes(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, map[string]any{"notes": a.Store.Notes()})
}

func (a *API) history(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, map[string]any{"history": a.Store.History(0)})
}

// ttsPreview returns a WAV of ?text= in Tarquin's voice (handy for checking the clone).
func (a *API) ttsPreview(w http.ResponseWriter, r *http.Request) {
	text := r.URL.Query().Get("text")
	if text == "" {
		fail(w, 400, "text required")
		return
	}
	pcm, err := a.TTS.Synthesize(r.Context(), text)
	if err != nil {
		fail(w, 502, err.Error())
		return
	}
	w.Header().Set("Content-Type", "audio/wav")
	w.Write(wrapWAV(pcm, a.Cfg.AudioRate))
}
