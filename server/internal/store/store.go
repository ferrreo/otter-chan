// Package store keeps bots, inboxes, notes, timers and chat history, persisted as JSON.
package store

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

var Activities = map[string]bool{"idle": true, "coding": true, "browsing": true, "thinking": true, "talking": true, "writing": true, "error": true, "offline": true, "done": true, "waiting": true}

type Bot struct {
	Name     string  `json:"name"`
	Activity string  `json:"activity"`
	Detail   string  `json:"detail"`
	Emoji    string  `json:"emoji"`
	Updated  float64 `json:"updated"`
}

type Message struct {
	ID    string  `json:"id"`
	TS    float64 `json:"ts"`
	From  string  `json:"from"`
	To    string  `json:"to"`
	Text  string  `json:"text"`
	Acked bool    `json:"acked"`
}

type Note struct {
	TS   float64 `json:"ts"`
	Text string  `json:"text"`
}

type Timer struct {
	ID    string  `json:"id"`
	Due   float64 `json:"due"`
	Label string  `json:"label"`
}

type Turn struct {
	Role    string  `json:"role"`
	Content string  `json:"content"`
	TS      float64 `json:"ts"`
}

type Event struct {
	Event string      `json:"event"`
	Bot   string      `json:"bot,omitempty"`
	Data  interface{} `json:"data,omitempty"`
}

type data struct {
	Bots    map[string]*Bot      `json:"bots"`
	Inbox   map[string][]Message `json:"inbox"`
	Notes   []Note               `json:"notes"`
	Timers  []Timer              `json:"timers"`
	History []Turn               `json:"history"`
}

type Store struct {
	path string
	mu   sync.Mutex
	d    data

	lmu       sync.Mutex
	listeners map[chan Event]struct{}
}

func now() float64 { return float64(time.Now().UnixNano()) / 1e9 }

func id() string {
	b := make([]byte, 5)
	rand.Read(b)
	return hex.EncodeToString(b)
}

func Open(dir string) *Store {
	s := &Store{path: filepath.Join(dir, "state.json"), listeners: map[chan Event]struct{}{}}
	s.d = data{Bots: map[string]*Bot{}, Inbox: map[string][]Message{}}
	if raw, err := os.ReadFile(s.path); err == nil {
		json.Unmarshal(raw, &s.d)
		if s.d.Bots == nil {
			s.d.Bots = map[string]*Bot{}
		}
		if s.d.Inbox == nil {
			s.d.Inbox = map[string][]Message{}
		}
	}
	return s
}

func (s *Store) save() {
	if len(s.d.History) > 40 {
		s.d.History = s.d.History[len(s.d.History)-40:]
	}
	if len(s.d.Notes) > 200 {
		s.d.Notes = s.d.Notes[len(s.d.Notes)-200:]
	}
	for k, v := range s.d.Inbox {
		if len(v) > 100 {
			s.d.Inbox[k] = v[len(v)-100:]
		}
	}
	raw, _ := json.MarshalIndent(s.d, "", " ")
	tmp := s.path + ".tmp"
	if os.WriteFile(tmp, raw, 0o644) == nil {
		os.Rename(tmp, s.path)
	}
}

// ---- bots ----

func (s *Store) bot(name string) *Bot {
	b := s.d.Bots[name]
	if b == nil {
		b = &Bot{Name: name, Activity: "idle", Updated: now()}
		s.d.Bots[name] = b
	}
	return b
}

func (s *Store) Touch(name string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.bot(name)
}

func (s *Store) SetStatus(name, activity, detail, emoji string) Bot {
	s.mu.Lock()
	defer s.mu.Unlock()
	b := s.bot(name)
	if !Activities[activity] {
		activity = "idle"
	}
	b.Activity, b.Detail, b.Emoji, b.Updated = activity, trunc(detail, 60), trunc(emoji, 4), now()
	s.save()
	cp := *b
	s.notify(Event{Event: "status", Bot: name, Data: cp})
	return cp
}

func trunc(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[:n])
	}
	return s
}

type BotView struct {
	Name     string `json:"name"`
	Activity string `json:"activity"`
	Detail   string `json:"detail"`
	Emoji    string `json:"emoji"`
	AgeS     int    `json:"age_s"`
}

// Bots returns bots in name order (the robot's cards must not shuffle); stale ones read as offline.
func (s *Store) Bots(limit int) []BotView {
	s.mu.Lock()
	defer s.mu.Unlock()
	list := make([]*Bot, 0, len(s.d.Bots))
	for _, b := range s.d.Bots {
		list = append(list, b)
	}
	// Busy bots first (so an idle heartbeat doesn't push an active chip off the small screen), then newest.
	sort.Slice(list, func(i, j int) bool {
		bi := list[i].Activity != "idle" && list[i].Activity != "offline" && list[i].Activity != "done"
		bj := list[j].Activity != "idle" && list[j].Activity != "offline" && list[j].Activity != "done"
		if bi != bj {
			return bi
		}
		return list[i].Name < list[j].Name // stable within a group: the robot's cards must not shuffle
	})
	out := []BotView{}
	for i, b := range list {
		if limit > 0 && i >= limit {
			break
		}
		age := now() - b.Updated
		act := b.Activity
		if age > 30*60 && act != "offline" && act != "error" {
			act = "offline"
		}
		out = append(out, BotView{b.Name, act, b.Detail, b.Emoji, int(age)})
	}
	return out
}

// ---- messages ----

func (s *Store) SendToBot(bot, text, from string) Message {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.bot(bot)
	m := Message{ID: id(), TS: now(), From: from, To: bot, Text: text}
	s.d.Inbox[bot] = append(s.d.Inbox[bot], m)
	s.save()
	s.notify(Event{Event: "message", Bot: bot, Data: m})
	return m
}

func (s *Store) Pending(bot string) []Message {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := []Message{}
	for _, m := range s.d.Inbox[bot] {
		if !m.Acked {
			out = append(out, m)
		}
	}
	return out
}

func (s *Store) Ack(bot string, ids []string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	want := map[string]bool{}
	for _, i := range ids {
		want[i] = true
	}
	n := 0
	list := s.d.Inbox[bot]
	for i := range list {
		if !list[i].Acked && (ids == nil || want[list[i].ID]) {
			list[i].Acked = true
			n++
		}
	}
	s.save()
	return n
}

// ---- notes / timers / history ----

func (s *Store) Remember(text string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.d.Notes = append(s.d.Notes, Note{now(), trunc(text, 400)})
	s.save()
}

func (s *Store) Notes() []Note {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]Note{}, s.d.Notes...)
}

func (s *Store) AddTimer(seconds int, label string) Timer {
	s.mu.Lock()
	defer s.mu.Unlock()
	t := Timer{ID: id(), Due: now() + float64(seconds), Label: trunc(label, 120)}
	s.d.Timers = append(s.d.Timers, t)
	s.save()
	return t
}

func (s *Store) Timers() []Timer {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]Timer{}, s.d.Timers...)
}

func (s *Store) PopDueTimers() []Timer {
	s.mu.Lock()
	defer s.mu.Unlock()
	var due, keep []Timer
	n := now()
	for _, t := range s.d.Timers {
		if t.Due <= n {
			due = append(due, t)
		} else {
			keep = append(keep, t)
		}
	}
	if len(due) > 0 {
		s.d.Timers = keep
		s.save()
	}
	return due
}

func (s *Store) AddHistory(role, content string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.d.History = append(s.d.History, Turn{role, trunc(content, 2000), now()})
	s.save()
}

func (s *Store) History(n int) []Turn {
	s.mu.Lock()
	defer s.mu.Unlock()
	h := s.d.History
	if n > 0 && len(h) > n {
		h = h[len(h)-n:]
	}
	return append([]Turn{}, h...)
}

// ---- event fan-out (long-poll) ----

func (s *Store) Subscribe() chan Event {
	ch := make(chan Event, 16)
	s.lmu.Lock()
	s.listeners[ch] = struct{}{}
	s.lmu.Unlock()
	return ch
}

func (s *Store) Unsubscribe(ch chan Event) {
	s.lmu.Lock()
	delete(s.listeners, ch)
	s.lmu.Unlock()
}

func (s *Store) notify(e Event) {
	s.lmu.Lock()
	defer s.lmu.Unlock()
	for ch := range s.listeners {
		select {
		case ch <- e:
		default:
		}
	}
}
