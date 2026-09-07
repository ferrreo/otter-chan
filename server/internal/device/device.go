// Package device holds the robot's WebSocket session and the listen -> STT -> LLM -> TTS pipeline.
package device

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"

	"otter-chan/server/internal/config"
	"otter-chan/server/internal/llm"
	"otter-chan/server/internal/store"
	"otter-chan/server/internal/stt"
	"otter-chan/server/internal/tts"
	"otter-chan/server/internal/vision"
	"otter-chan/server/internal/wake"
)

// dismissRe catches dismissals even if the model forgets to call end_conversation.
var dismissRe = regexp.MustCompile(`(?i)\b(that('ll| will| would) be all|that's (all|everything)|thanks?,? that's (all|it)|dismissed|good ?bye|good night|go (back )?to sleep|nothing (else|more))\b`)

const (
	BinAudio     = 0x01
	BinWakeClip  = 0x02
	BinPhoto     = 0x03
	BinTrackJPEG = 0x04

	chunkBytes  = 4096 // 128 ms of 16 kHz s16le
	leadSeconds = 3.0  // how far ahead of real time playback may run (robot buffers ~6 s)
)

type Deps struct {
	Cfg   *config.Config
	Store *store.Store
	STT   *stt.Client
	TTS   tts.Synth
	LLM   *llm.Client
}

// Hub tracks the single connected robot.
type Hub struct {
	deps Deps
	mu   sync.RWMutex
	cur  *Session
}

func NewHub(d Deps) *Hub { return &Hub{deps: d} }

func (h *Hub) Current() *Session {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.cur
}

type queued struct {
	text, expr string
	chime      bool
	from       string
}

type Session struct {
	hub  *Hub
	ws   *websocket.Conn
	wmu  sync.Mutex
	Name string

	state     atomic.Value // string
	Telemetry atomic.Value // map[string]any

	umu       sync.Mutex
	utterance []byte
	listening bool

	pipeMu   sync.Mutex
	pipeCtx  context.Context
	pipeStop context.CancelFunc
	busy     atomic.Bool
	cancel   atomic.Bool

	wakeBusy  atomic.Bool
	trackBusy atomic.Bool

	photoMu sync.Mutex
	photoCh chan []byte

	queue chan queued
	done  chan struct{}

	sentBytes     atomic.Int64 // PCM bytes sent since say_start
	endAfterReply atomic.Bool

	pendMu      sync.Mutex
	pendingText string // question captured inside the wake clip; answered on the next listen_start
}

func (h *Hub) Serve(ws *websocket.Conn, name string) {
	s := &Session{hub: h, ws: ws, Name: name, queue: make(chan queued, 16), done: make(chan struct{})}
	s.state.Store("unknown")
	s.Telemetry.Store(map[string]any{})
	h.mu.Lock()
	old := h.cur
	h.cur = s
	h.mu.Unlock()
	if old != nil {
		old.ws.Close()
	}
	log.Printf("device connected: %s", name)
	go s.background()
	s.readLoop()
	close(s.done)
	h.mu.Lock()
	if h.cur == s {
		h.cur = nil
	}
	h.mu.Unlock()
	log.Printf("device disconnected: %s", name)
}

func (s *Session) State() string { return s.state.Load().(string) }
func (s *Session) Busy() bool    { return s.busy.Load() }

// ---------------------------------------------------------------- send helpers

func (s *Session) SendJSON(obj map[string]any) error {
	raw, _ := json.Marshal(obj)
	s.wmu.Lock()
	defer s.wmu.Unlock()
	s.ws.SetWriteDeadline(time.Now().Add(10 * time.Second))
	return s.ws.WriteMessage(websocket.TextMessage, raw)
}

func (s *Session) J(typ string, kv ...any) map[string]any {
	m := map[string]any{"type": typ}
	for i := 0; i+1 < len(kv); i += 2 {
		m[kv[i].(string)] = kv[i+1]
	}
	return m
}

func (s *Session) sendBin(tag byte, data []byte) error {
	s.wmu.Lock()
	defer s.wmu.Unlock()
	s.ws.SetWriteDeadline(time.Now().Add(10 * time.Second))
	return s.ws.WriteMessage(websocket.BinaryMessage, append([]byte{tag}, data...))
}

func (s *Session) PushBots() { s.SendJSON(s.J("bots", "bots", s.hub.deps.Store.Bots(6))) }

// ---------------------------------------------------------------- inbound

func (s *Session) readLoop() {
	s.ws.SetReadLimit(8 << 20)
	for {
		mt, data, err := s.ws.ReadMessage()
		if err != nil {
			return
		}
		switch mt {
		case websocket.TextMessage:
			var d map[string]any
			if json.Unmarshal(data, &d) == nil {
				s.onJSON(d)
			}
		case websocket.BinaryMessage:
			if len(data) > 0 {
				s.onBin(data[0], data[1:])
			}
		}
	}
}

func str(m map[string]any, k string) string {
	if v, ok := m[k].(string); ok {
		return v
	}
	return ""
}

func (s *Session) onJSON(d map[string]any) {
	switch str(d, "type") {
	case "hello":
		if n := str(d, "name"); n != "" {
			s.Name = n
		}
		if m := str(d, "mode"); m != "" {
			s.state.Store(m)
		}
		s.SendJSON(s.J("hello_ack", "name", s.hub.deps.Cfg.Name, "wake_phrases", s.hub.deps.Cfg.WakePhrases))
		s.PushBots()
	case "state":
		s.state.Store(str(d, "state"))
	case "listen_start":
		s.stopPipeline()
		s.umu.Lock()
		s.utterance = s.utterance[:0]
		s.listening = true
		s.umu.Unlock()
		s.cancel.Store(false)
		s.pendMu.Lock()
		pending := s.pendingText
		s.pendingText = ""
		s.pendMu.Unlock()
		if pending != "" {
			// the wake clip already contained the question: answer it instead of listening again
			s.umu.Lock()
			s.listening = false
			s.umu.Unlock()
			s.startPipeline(func(ctx context.Context) {
				s.SendJSON(s.J("thinking"))
				s.SendJSON(s.J("transcript", "text", pending, "final", true))
				if _, err := s.RespondTo(ctx, pending, nil); err != nil && !errors.Is(err, context.Canceled) {
					s.fail(err)
				}
			})
		}
	case "listen_end":
		s.umu.Lock()
		s.listening = false
		pcm := append([]byte{}, s.utterance...)
		s.utterance = s.utterance[:0]
		s.umu.Unlock()
		s.startPipeline(func(ctx context.Context) { s.pipeline(ctx, pcm) })
	case "listen_cancel":
		s.umu.Lock()
		s.listening = false
		s.utterance = s.utterance[:0]
		s.umu.Unlock()
	case "cancel":
		s.cancel.Store(true)
		s.stopPipeline()
	case "telemetry":
		s.Telemetry.Store(d)
	case "event":
		s.onEvent(str(d, "name"))
	case "photo_failed":
		s.photoMu.Lock()
		if s.photoCh != nil {
			close(s.photoCh)
			s.photoCh = nil
		}
		s.photoMu.Unlock()
	}
}

func (s *Session) onBin(tag byte, payload []byte) {
	switch tag {
	case BinAudio:
		s.umu.Lock()
		if s.listening && len(s.utterance) < s.hub.deps.Cfg.AudioRate*2*30 {
			s.utterance = append(s.utterance, payload...)
		}
		s.umu.Unlock()
	case BinWakeClip:
		if !s.busy.Load() && s.wakeBusy.CompareAndSwap(false, true) {
			clip := append([]byte{}, payload...)
			go func() {
				defer s.wakeBusy.Store(false)
				s.wakeCheck(clip)
			}()
		}
	case BinPhoto:
		s.photoMu.Lock()
		if s.photoCh != nil {
			s.photoCh <- append([]byte{}, payload...)
			s.photoCh = nil
		}
		s.photoMu.Unlock()
	case BinTrackJPEG:
		if s.trackBusy.CompareAndSwap(false, true) {
			jpg := append([]byte{}, payload...)
			go func() {
				defer s.trackBusy.Store(false)
				r := vision.FindFace(jpg)
				s.SendJSON(map[string]any{"type": "face", "found": r.Found, "x": r.X, "y": r.Y, "w": r.W})
			}()
		}
	}
}

func (s *Session) wakeCheck(pcm []byte) {
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	text, err := s.hub.deps.STT.Transcribe(ctx, pcm)
	if err != nil {
		log.Printf("wake stt: %v", err)
		return
	}
	ok, score := wake.Matches(text, s.hub.deps.Cfg.WakePhrases, s.hub.deps.Cfg.WakeThreshold)
	log.Printf("wake clip -> %q score=%d wake=%v", text, score, ok)
	if ok && s.State() == "standby" {
		if rest := wake.Remainder(text, s.hub.deps.Cfg.WakePhrases); rest != "" {
			s.pendMu.Lock()
			s.pendingText = rest
			s.pendMu.Unlock()
		}
		s.SendJSON(s.J("wake"))
	}
}

func (s *Session) onEvent(name string) {
	switch name {
	case "shake":
		s.Announce("[surprised] Steady on, sir. I am not a cocktail.", "", false)
	}
}

// ---------------------------------------------------------------- pipeline

func (s *Session) startPipeline(fn func(ctx context.Context)) {
	s.stopPipeline()
	ctx, stop := context.WithCancel(context.Background())
	s.pipeMu.Lock()
	s.pipeCtx, s.pipeStop = ctx, stop
	s.pipeMu.Unlock()
	s.busy.Store(true)
	go func() {
		defer func() {
			s.busy.Store(false)
			stop()
		}()
		fn(ctx)
	}()
}

func (s *Session) stopPipeline() {
	s.pipeMu.Lock()
	if s.pipeStop != nil {
		s.pipeStop()
		s.pipeStop = nil
	}
	s.pipeMu.Unlock()
}

func (s *Session) pipeline(ctx context.Context, pcm []byte) {
	s.SendJSON(s.J("thinking"))
	sttCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
	text, err := s.hub.deps.STT.Transcribe(sttCtx, pcm)
	cancel()
	if err != nil {
		s.fail(err)
		return
	}
	if len(s.hub.deps.Cfg.WakePhrases) > 0 {
		text = wake.Normalize(text, strings.Title(strings.Fields(s.hub.deps.Cfg.WakePhrases[0])[len(strings.Fields(s.hub.deps.Cfg.WakePhrases[0]))-1]))
	}
	log.Printf("heard: %q", text)
	if len(strings.Trim(text, ".,!? ")) < 2 {
		s.SendJSON(s.J("nothing_heard"))
		return
	}
	s.SendJSON(s.J("transcript", "text", text, "final", true))
	if _, err := s.RespondTo(ctx, text, nil); err != nil && !errors.Is(err, context.Canceled) {
		s.fail(err)
	}
}

func (s *Session) fail(err error) {
	log.Printf("pipeline failed: %v", err)
	msg := err.Error()
	if len(msg) > 60 {
		msg = msg[:60]
	}
	s.SendJSON(s.J("expression", "name", "error", "ms", 2500))
	s.SendJSON(s.J("caption", "text", "error: "+msg, "ms", 5000))
	s.SendJSON(s.J("cancel"))
}

// RespondTo runs the LLM on text and streams the spoken reply. Returns the reply text.
func (s *Session) RespondTo(ctx context.Context, text string, images [][]byte) (string, error) {
	followup := s.hub.deps.Cfg.Followup
	started := false
	var sayErr error
	// Sentences are synthesised as soon as the model emits them (in order), and streamed one behind:
	// the next sentence renders while the current one plays, so there is no gap between them.
	type job struct {
		sentence, expr string
		pcm            chan []byte
	}
	jobs := make(chan job, 8)
	go func() {
		for j := range jobs {
			if s.cancel.Load() {
				close(j.pcm)
				continue
			}
			pcm, err := s.hub.deps.TTS.Synthesize(ctx, j.sentence)
			if err != nil {
				log.Printf("tts: %v", err)
				pcm = nil
			}
			j.pcm <- pcm
			close(j.pcm)
		}
	}()
	var pending []job
	drain := func(n int) {
		for len(pending) > n {
			j := pending[0]
			pending = pending[1:]
			pcm := <-j.pcm
			if s.cancel.Load() || sayErr != nil {
				continue
			}
			if !started {
				s.sentBytes.Store(0)
				s.SendJSON(s.J("say_start", "text", j.sentence, "expression", j.expr, "followup", followup))
				started = true
			} else {
				s.SendJSON(s.J("caption", "text", j.sentence, "ms", 0))
			}
			if pcm == nil {
				sayErr = fmt.Errorf("tts failed")
				continue
			}
			sayErr = s.streamPCM(ctx, pcm)
		}
	}
	reply, err := s.hub.deps.LLM.Respond(ctx, text, images, s.runTool, func(expr, sentence string) {
		if s.cancel.Load() || sayErr != nil {
			return
		}
		j := job{sentence: sentence, expr: expr, pcm: make(chan []byte, 1)}
		jobs <- j
		pending = append(pending, j)
		drain(1) // keep exactly one sentence rendering ahead
	})
	close(jobs)
	drain(0)
	if !started {
		s.SendJSON(s.J("say_start", "text", "", "expression", "neutral", "followup", false))
	}
	end := s.endAfterReply.Swap(false) || dismissRe.MatchString(text)
	s.SendJSON(s.J("say_end", "followup", followup && started && !end, "end", end, "bytes", s.sentBytes.Load()))
	if err == nil {
		err = sayErr
	}
	return reply, err
}

// speak says text (with optional leading [tag]) right now.
func (s *Session) speak(ctx context.Context, text, expr string, followup bool) error {
	e, body := llm.SplitExpression(text)
	if expr == "" {
		expr = e
	}
	s.SendJSON(s.J("say_start", "text", body, "expression", expr, "followup", followup))
	var err error
	for _, sen := range llm.Sentences(body) {
		if err = s.streamTTS(ctx, sen); err != nil {
			break
		}
	}
	s.SendJSON(s.J("say_end", "followup", followup))
	return err
}

// Announce speaks now if idle, otherwise queues (bot messages, timers, API /say).
func (s *Session) Announce(text, from string, chime bool) {
	e, body := llm.SplitExpression(text)
	if chime {
		n := s.J("notify", "text", trunc(body, 80))
		if from != "" {
			n["from"] = from
		}
		s.SendJSON(n)
	}
	select {
	case s.queue <- queued{text: "[" + e + "] " + body, expr: e, chime: chime, from: from}:
	default:
		log.Printf("announce queue full; dropped")
	}
}

func trunc(s string, n int) string {
	if r := []rune(s); len(r) > n {
		return string(r[:n])
	}
	return s
}

func (s *Session) streamTTS(ctx context.Context, sentence string) error {
	pcm, err := s.hub.deps.TTS.Synthesize(ctx, sentence)
	if err != nil {
		return fmt.Errorf("tts: %w", err)
	}
	return s.streamPCM(ctx, pcm)
}

// streamPCM sends PCM in chunks, paced so the robot's buffer stays leadSeconds ahead of real time.
func (s *Session) streamPCM(ctx context.Context, pcm []byte) error {
	defer s.sentBytes.Add(int64(len(pcm)))
	t0 := time.Now()
	sent := 0.0
	rate := float64(s.hub.deps.Cfg.AudioRate)
	for i := 0; i < len(pcm); i += chunkBytes {
		if s.cancel.Load() || ctx.Err() != nil {
			return context.Canceled
		}
		end := i + chunkBytes
		if end > len(pcm) {
			end = len(pcm)
		}
		if err := s.sendBin(BinAudio, pcm[i:end]); err != nil {
			return err
		}
		sent += float64(end-i) / 2 / rate
		if ahead := sent - time.Since(t0).Seconds(); ahead > leadSeconds {
			time.Sleep(time.Duration((ahead - leadSeconds) * float64(time.Second)))
		}
	}
	return nil
}

func (s *Session) background() {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	lastBots := time.Time{}
	for {
		select {
		case <-s.done:
			return
		case <-tick.C:
		}
		if time.Since(lastBots) > 20*time.Second {
			s.PushBots()
			lastBots = time.Now()
		}
		for _, t := range s.hub.deps.Store.PopDueTimers() {
			s.Announce("[surprised] Your reminder, sir: "+t.Label+".", "", true)
		}
		st := s.State()
		if (st == "standby" || st == "muted") && !s.busy.Load() {
			select {
			case q := <-s.queue:
				s.startPipeline(func(ctx context.Context) { s.speak(ctx, q.text, q.expr, false) })
			default:
			}
		}
	}
}

// ---------------------------------------------------------------- LLM tools

func num(args map[string]any, k string, def float64) float64 {
	if v, ok := args[k].(float64); ok {
		return v
	}
	return def
}

func (s *Session) runTool(ctx context.Context, name string, args map[string]any) (any, []byte) {
	st := s.hub.deps.Store
	switch name {
	case "send_message_to_bot":
		target := str(args, "bot")
		var bots []string
		if target == "all" {
			for _, b := range st.Bots(0) {
				bots = append(bots, b.Name)
			}
		} else if target != "" {
			bots = []string{target}
		}
		if len(bots) == 0 {
			return map[string]any{"error": "no such bot", "known": st.Bots(0)}, nil
		}
		for _, b := range bots {
			st.SendToBot(b, str(args, "message"), "user")
		}
		return map[string]any{"ok": true, "delivered_to": bots}, nil
	case "get_bots":
		return map[string]any{"bots": st.Bots(0)}, nil
	case "take_photo":
		jpg, err := s.RequestPhoto(ctx, 20)
		if err != nil {
			return map[string]any{"error": err.Error()}, nil
		}
		return nil, jpg
	case "gesture":
		s.SendJSON(s.J("gesture", "name", str(args, "name")))
	case "set_lights":
		s.SendJSON(s.J("led", "mode", str(args, "mode"), "r", int(num(args, "r", 0)), "g", int(num(args, "g", 0)), "b", int(num(args, "b", 0)), "ms", int(num(args, "seconds", 0))*1000))
	case "look":
		s.SendJSON(s.J("look", "x", num(args, "x", 0), "y", num(args, "y", 0)))
	case "set_timer":
		t := st.AddTimer(int(num(args, "seconds", 60)), str(args, "label"))
		return map[string]any{"ok": true, "timer": t}, nil
	case "remember":
		st.Remember(str(args, "note"))
	case "set_volume":
		pct := int(num(args, "percent", 60))
		if pct < 0 {
			pct = 0
		} else if pct > 100 {
			pct = 100
		}
		s.SendJSON(s.J("config", "volume", pct*255/100))
	case "go_to_sleep":
		time.AfterFunc(5*time.Second, func() { s.SendJSON(s.J("sleep")) })
		return map[string]any{"ok": true, "note": "will sleep after this reply"}, nil
	case "set_tracking":
		en, _ := args["enabled"].(bool)
		s.SendJSON(s.J("config", "tracking", en))
	case "end_conversation":
		s.endAfterReply.Store(true)
	default:
		return map[string]any{"error": "unknown tool " + name}, nil
	}
	return map[string]any{"ok": true}, nil
}

// RequestPhoto asks the robot for a JPEG.
func (s *Session) RequestPhoto(ctx context.Context, quality int) ([]byte, error) {
	ch := make(chan []byte, 1)
	s.photoMu.Lock()
	s.photoCh = ch
	s.photoMu.Unlock()
	s.SendJSON(s.J("photo_request", "quality", quality))
	select {
	case jpg, ok := <-ch:
		if !ok || jpg == nil {
			return nil, errors.New("camera unavailable")
		}
		return jpg, nil
	case <-time.After(8 * time.Second):
		s.photoMu.Lock()
		s.photoCh = nil
		s.photoMu.Unlock()
		return nil, errors.New("photo timeout")
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

// ToolRunner exposes the session's tools to callers outside the pipeline (e.g. /api/ask).
func (s *Session) ToolRunner() llm.ToolRunner { return s.runTool }
