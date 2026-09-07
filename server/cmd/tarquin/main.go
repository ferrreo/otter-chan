// Tarquin: server for the Otter-chan / StackChan robot butler.
package main

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"strconv"
	"time"

	"github.com/gorilla/websocket"

	"otter-chan/server/internal/api"
	"otter-chan/server/internal/auth"
	"otter-chan/server/internal/config"
	"otter-chan/server/internal/device"
	"otter-chan/server/internal/llm"
	"otter-chan/server/internal/store"
	"otter-chan/server/internal/stt"
	"otter-chan/server/internal/tts"
)

var upgrader = websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }, ReadBufferSize: 8192, WriteBufferSize: 8192}

func main() {
	cfg, err := config.Load()
	if err != nil {
		log.Fatal(err)
	}
	st := store.Open(cfg.DataDir)
	sttc := stt.New(cfg.STTURL, cfg.AudioRate)
	var ttse tts.Synth
	if cfg.TTSEngine == "vox" {
		ttse = tts.New(cfg.VoxBin, cfg.VoxVoice, cfg.VoxArgs, strconv.Itoa(cfg.AudioRate), cfg.DataDir+"/tts_cache")
	} else {
		ttse = tts.NewPiper(cfg.PiperBin, cfg.PiperModel, cfg.AudioRate, cfg.DataDir+"/tts_cache", cfg.PiperSpeed)
	}
	brain := llm.New(cfg.LLMBaseURL, cfg.LLMModel, cfg.LLMReasoning, cfg.LLMAPIKey, cfg.Name, st)
	hub := device.NewHub(device.Deps{Cfg: cfg, Store: st, STT: sttc, TTS: ttse, LLM: brain})
	authz := auth.Resolver{Tokens: cfg.Tokens}

	mux := http.NewServeMux()
	apiMux := http.NewServeMux()
	(&api.API{Cfg: cfg, Store: st, Hub: hub, STT: sttc, TTS: ttse, LLM: brain}).Routes(apiMux)
	mux.Handle("/api/", authz.Middleware(apiMux))
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, r *http.Request) {
		s := hub.Current()
		state := "none"
		if s != nil {
			state = s.State()
		}
		fmt.Fprintf(w, `{"ok":true,"robot":%v,"state":%q}`, s != nil, state)
	})
	mux.HandleFunc("GET /{$}", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, "<h2>Tarquin server</h2><p>Robot WebSocket: <code>/ws/device</code>. Bot API under <code>/api</code> (bearer token). See docs/bot-api.md.</p>")
	})
	mux.HandleFunc("GET /ws/device", func(w http.ResponseWriter, r *http.Request) {
		if authz.Identity(r) != "device" {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		ws, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		name := r.Header.Get("X-Otter-Name")
		if name == "" {
			name = "tarquin"
		}
		hub.Serve(ws, name)
	})

	addr := fmt.Sprintf("%s:%d", cfg.Host, cfg.Port)
	log.Printf("Tarquin listening on %s — model %s, tts %s, stt %s", addr, cfg.LLMModel, ttse.Name(), cfg.STTURL)
	if cfg.LLMAPIKey == "" {
		log.Printf("WARNING: no Meta API key (META_MODEL_API_KEY / ~/.config/muse/auth.json)")
	}
	go warm(ttse)
	srv := &http.Server{Addr: addr, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	log.Fatal(srv.ListenAndServe())
}

// warm starts the resident TTS process so the first reply doesn't pay the model load.
func warm(t tts.Synth) {
	if err := t.Warm(context.Background()); err != nil {
		log.Printf("tts warm-up: %v", err)
	}
}
