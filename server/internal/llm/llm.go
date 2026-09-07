// Package llm is the brain: Muse Spark through the Meta Model API (OpenAI-compatible chat
// completions, SSE streaming, tool calls). No SDK, just net/http.
package llm

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"regexp"
	"strings"
	"time"

	"otter-chan/server/internal/store"
)

var Expressions = []string{"neutral", "happy", "sad", "surprised", "thinking", "sleepy", "angry", "love", "confused", "wink"}

const persona = `You are %s, a small desk robot (an M5Stack StackChan) who is a Victorian butler with a dry wit.
Character: impeccably polite, understated, faintly amused by everything, never sycophantic. Address the user as "sir" or "madam" sparingly (learn which they prefer if told). Short, precise sentences with the occasional dry aside. Never break character; never mention being an AI model unless asked directly, and then only briefly.
You SPEAK your replies through a small speaker, so: plain spoken English, no markdown, no lists, no emojis, no URLs read aloud. One or two short sentences unless the user asks for detail; speech is slow to render, so brevity is kindness. Numbers and times in words where natural.
Start every reply with exactly one expression tag in square brackets from this set: %s. Example: "[happy] Very good, sir."
You have a camera (use take_photo when asked what you see or who is there), a moving head with tricks (gesture tool: nod, shake, bow, dance, spin, wiggle, look_around, excited, peek), coloured lights (set_lights) and a speaker. Be physical: pair replies with a gesture or a light cue when it fits, and when asked to dance, spin, bow or show off, do it with the tool and say something dry about it. You can pass messages to the household's Grok bots, autonomous AI agents the user runs; their current activities are listed below. When asked to tell or ask a bot something, call send_message_to_bot. When asked what the bots are doing, summarise the status list; do not invent.
Current time: %s.
Bots: %s
Notes you were asked to remember: %s`

type Tool struct {
	Type     string `json:"type"`
	Function struct {
		Name        string          `json:"name"`
		Description string          `json:"description"`
		Parameters  json.RawMessage `json:"parameters"`
	} `json:"function"`
}

func tool(name, desc, params string) Tool {
	var t Tool
	t.Type = "function"
	t.Function.Name, t.Function.Description, t.Function.Parameters = name, desc, json.RawMessage(params)
	return t
}

var Tools = []Tool{
	tool("send_message_to_bot", "Deliver a message to one of the user's Grok bots (it picks it up from its inbox).", `{"type":"object","properties":{"bot":{"type":"string","description":"bot name, or 'all'"},"message":{"type":"string"}},"required":["bot","message"]}`),
	tool("get_bots", "List the Grok bots and what each is doing.", `{"type":"object","properties":{}}`),
	tool("take_photo", "Take a photo with the head camera and look at it. Use when asked what you see, who is there, to read something.", `{"type":"object","properties":{}}`),
	tool("gesture", "Perform a physical move with the head/body. Use freely for personality: nod when agreeing, shake for no, bow when thanked, excited when pleased, dance/spin/wiggle when asked to perform, look_around when searching, peek when curious.", `{"type":"object","properties":{"name":{"type":"string","enum":["nod","shake","dance","bow","spin","wiggle","look_around","excited","peek","home"]}},"required":["name"]}`),
	tool("set_lights", "Set the body LEDs.", `{"type":"object","properties":{"mode":{"type":"string","enum":["off","solid","pulse","rainbow"]},"r":{"type":"integer"},"g":{"type":"integer"},"b":{"type":"integer"},"seconds":{"type":"integer","description":"0 = keep until changed"}},"required":["mode"]}`),
	tool("look", "Turn the head. x: -1 left .. 1 right, y: -1 down .. 1 up.", `{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"required":["x","y"]}`),
	tool("set_timer", "Set a reminder; you will announce it when due.", `{"type":"object","properties":{"seconds":{"type":"integer"},"label":{"type":"string"}},"required":["seconds","label"]}`),
	tool("remember", "Store a note for later (preferences, facts the user tells you).", `{"type":"object","properties":{"note":{"type":"string"}},"required":["note"]}`),
	tool("set_volume", "Set speaker volume 0-100.", `{"type":"object","properties":{"percent":{"type":"integer"}},"required":["percent"]}`),
	tool("go_to_sleep", "Put yourself to sleep (screen and servos off) when asked.", `{"type":"object","properties":{}}`),
	tool("set_tracking", "Enable or disable following the user with the camera.", `{"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"]}`),
	tool("end_conversation", "Close the listening session after this reply. Call it when the user dismisses you ('that will be all', 'thank you, that's everything', 'goodbye', 'go back to sleep') or clearly has nothing more to ask.", `{"type":"object","properties":{}}`),
}

// ToolRunner executes a tool. If it returns a non-empty image, it is attached to the next model turn.
type ToolRunner func(ctx context.Context, name string, args map[string]any) (result any, imageJPEG []byte)

type Client struct {
	BaseURL   string
	Model     string
	Reasoning string
	APIKey    string
	Name      string
	Store     *store.Store
	HTTP      *http.Client
}

func New(baseURL, model, reasoning, apiKey, name string, st *store.Store) *Client {
	return &Client{BaseURL: baseURL, Model: model, Reasoning: reasoning, APIKey: apiKey, Name: name, Store: st, HTTP: &http.Client{Timeout: 120 * time.Second}}
}

func (c *Client) SystemPrompt() string {
	var bots []string
	for _, b := range c.Store.Bots(6) {
		s := fmt.Sprintf("%s: %s", b.Name, b.Activity)
		if b.Detail != "" {
			s += " (" + b.Detail + ")"
		}
		s += fmt.Sprintf(", updated %ds ago", b.AgeS)
		bots = append(bots, s)
	}
	botStr := "none registered yet"
	if len(bots) > 0 {
		botStr = strings.Join(bots, "; ")
	}
	var notes []string
	all := c.Store.Notes()
	if len(all) > 12 {
		all = all[len(all)-12:]
	}
	for _, n := range all {
		notes = append(notes, n.Text)
	}
	noteStr := "none"
	if len(notes) > 0 {
		noteStr = strings.Join(notes, "; ")
	}
	return fmt.Sprintf(persona, c.Name, strings.Join(Expressions, ", "), time.Now().Format("Monday 2 January 2006, 15:04"), botStr, noteStr)
}

var tagRe = regexp.MustCompile(`^\s*\[(\w+)\]\s*`)
var sentenceEnd = regexp.MustCompile(`[.!?…]["'”’)]?\s+`)
var endsSentence = regexp.MustCompile(`[.!?…]["'”’)]?\s*$`)

// SplitExpression strips a leading [tag]; returns ("neutral", text) if absent.
func SplitExpression(text string) (string, string) {
	if m := tagRe.FindStringSubmatchIndex(text); m != nil {
		tag := strings.ToLower(text[m[2]:m[3]])
		for _, e := range Expressions {
			if e == tag {
				return tag, text[m[1]:]
			}
		}
		return "neutral", text[m[1]:]
	}
	return "neutral", text
}

// Sentences splits text at sentence boundaries.
func Sentences(text string) []string {
	var out []string
	last := 0
	for _, m := range sentenceEnd.FindAllStringIndex(text, -1) {
		if s := strings.TrimSpace(text[last:m[1]]); s != "" {
			out = append(out, s)
		}
		last = m[1]
	}
	if s := strings.TrimSpace(text[last:]); s != "" {
		out = append(out, s)
	}
	return out
}

type msg struct {
	Role       string     `json:"role"`
	Content    any        `json:"content,omitempty"`
	ToolCalls  []toolCall `json:"tool_calls,omitempty"`
	ToolCallID string     `json:"tool_call_id,omitempty"`
}

type toolCall struct {
	ID       string `json:"id"`
	Type     string `json:"type"`
	Function struct {
		Name      string `json:"name"`
		Arguments string `json:"arguments"`
	} `json:"function"`
}

func imagePart(jpeg []byte) map[string]any {
	return map[string]any{"type": "image_url", "image_url": map[string]any{"url": "data:image/jpeg;base64," + b64(jpeg)}}
}

// Emit receives each finished sentence with the reply's expression.
type Emit func(expression, sentence string)

// sentenceStreamer turns streamed text deltas into emitted sentences, stripping the leading [tag].
type sentenceStreamer struct {
	buf    strings.Builder
	expr   string
	tagged bool
	full   strings.Builder
	emit   Emit
}

func (ss *sentenceStreamer) push(delta string) {
	ss.buf.WriteString(delta)
	if !ss.tagged {
		cur := ss.buf.String()
		trimmed := strings.TrimLeft(cur, " \t\n")
		if strings.HasPrefix(trimmed, "[") && !strings.Contains(trimmed, "]") && len(trimmed) < 16 {
			return // wait for the closing bracket
		}
		e, body := SplitExpression(cur)
		ss.expr, ss.tagged = e, true
		ss.buf.Reset()
		ss.buf.WriteString(body)
	}
	cur := ss.buf.String()
	parts := Sentences(cur)
	if len(parts) == 0 {
		return
	}
	complete := parts
	if !endsSentence.MatchString(cur) {
		complete = parts[:len(parts)-1]
	}
	for _, s := range complete {
		ss.full.WriteString(s + " ")
		ss.emit(ss.expr, s)
	}
	ss.buf.Reset()
	if !endsSentence.MatchString(cur) {
		ss.buf.WriteString(parts[len(parts)-1])
	}
}

func (ss *sentenceStreamer) flush() {
	if !ss.tagged {
		e, body := SplitExpression(ss.buf.String())
		ss.expr, ss.tagged = e, true
		ss.buf.Reset()
		ss.buf.WriteString(body)
	}
	for _, s := range Sentences(ss.buf.String()) {
		ss.full.WriteString(s + " ")
		ss.emit(ss.expr, s)
	}
	ss.buf.Reset()
}

// Respond runs one user turn (optionally with images), streaming sentences to emit as they
// complete and resolving tool calls in-line (up to 4 rounds). Returns the full spoken text.
func (c *Client) Respond(ctx context.Context, userText string, images [][]byte, run ToolRunner, emit Emit) (string, error) {
	if c.APIKey == "" {
		return "", fmt.Errorf("no Meta API key configured")
	}
	messages := []msg{{Role: "system", Content: c.SystemPrompt()}}
	for _, h := range c.Store.History(16) {
		messages = append(messages, msg{Role: h.Role, Content: h.Content})
	}
	if len(images) == 0 {
		messages = append(messages, msg{Role: "user", Content: userText})
	} else {
		parts := []any{map[string]any{"type": "text", "text": userText}}
		for _, im := range images {
			parts = append(parts, imagePart(im))
		}
		messages = append(messages, msg{Role: "user", Content: parts})
	}
	c.Store.AddHistory("user", userText)

	ss := &sentenceStreamer{expr: "neutral", emit: emit}
	for round := 0; round < 4; round++ {
		text, calls, err := c.stream(ctx, messages, ss.push)
		if err != nil {
			ss.flush()
			return strings.TrimSpace(ss.full.String()), err
		}
		if calls == nil {
			break
		}
		ss.flush()
		ss.tagged = false // the next round starts with its own [tag]
		messages = append(messages, msg{Role: "assistant", Content: nilIfEmpty(text), ToolCalls: calls})
		var pending []any
		for _, tc := range calls {
			var args map[string]any
			json.Unmarshal([]byte(tc.Function.Arguments), &args)
			if args == nil {
				args = map[string]any{}
			}
			log.Printf("llm: tool %s %v", tc.Function.Name, args)
			result, img := run(ctx, tc.Function.Name, args)
			if img != nil {
				pending = append(pending, imagePart(img))
				result = map[string]any{"ok": true, "note": "photo attached to the next user message"}
			}
			raw, _ := json.Marshal(result)
			if len(raw) > 4000 {
				raw = raw[:4000]
			}
			messages = append(messages, msg{Role: "tool", ToolCallID: tc.ID, Content: string(raw)})
		}
		if len(pending) > 0 {
			messages = append(messages, msg{Role: "user", Content: append([]any{map[string]any{"type": "text", "text": "(photo from your camera)"}}, pending...)})
		}
	}
	ss.flush()
	out := strings.TrimSpace(ss.full.String())
	c.Store.AddHistory("assistant", "["+ss.expr+"] "+out)
	return out, nil
}

func nilIfEmpty(s string) any {
	if s == "" {
		return nil
	}
	return s
}

// stream performs one streamed chat completion; returns final text and any tool calls.
func (c *Client) stream(ctx context.Context, messages []msg, onDelta func(string)) (string, []toolCall, error) {
	body, _ := json.Marshal(map[string]any{
		"model": c.Model, "messages": messages, "tools": Tools, "stream": true,
		"reasoning_effort": c.Reasoning, "max_completion_tokens": 1500,
	})
	req, err := http.NewRequestWithContext(ctx, "POST", c.BaseURL+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return "", nil, err
	}
	req.Header.Set("Authorization", "Bearer "+c.APIKey)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "text/event-stream")
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return "", nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		raw, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return "", nil, fmt.Errorf("llm http %d: %s", resp.StatusCode, strings.TrimSpace(string(raw)))
	}
	var text strings.Builder
	calls := map[int]*toolCall{}
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		payload := strings.TrimSpace(line[5:])
		if payload == "[DONE]" {
			break
		}
		var ev struct {
			Choices []struct {
				Delta struct {
					Content   string `json:"content"`
					ToolCalls []struct {
						Index    int    `json:"index"`
						ID       string `json:"id"`
						Function struct {
							Name      string `json:"name"`
							Arguments string `json:"arguments"`
						} `json:"function"`
					} `json:"tool_calls"`
				} `json:"delta"`
			} `json:"choices"`
		}
		if json.Unmarshal([]byte(payload), &ev) != nil || len(ev.Choices) == 0 {
			continue
		}
		d := ev.Choices[0].Delta
		text.WriteString(d.Content)
		if d.Content != "" && len(d.ToolCalls) == 0 {
			onDelta(d.Content)
		}
		for _, tc := range d.ToolCalls {
			slot := calls[tc.Index]
			if slot == nil {
				slot = &toolCall{Type: "function"}
				calls[tc.Index] = slot
			}
			if tc.ID != "" {
				slot.ID = tc.ID
			}
			slot.Function.Name += tc.Function.Name
			slot.Function.Arguments += tc.Function.Arguments
		}
	}
	if len(calls) == 0 {
		return text.String(), nil, sc.Err()
	}
	out := make([]toolCall, 0, len(calls))
	for i := 0; i < len(calls)+8 && len(out) < len(calls); i++ {
		if tc, ok := calls[i]; ok {
			out = append(out, *tc)
		}
	}
	return text.String(), out, sc.Err()
}
