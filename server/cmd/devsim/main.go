// devsim pretends to be the robot: connects to the server, sends an utterance from a WAV file, and
// writes the spoken reply to a WAV. Handy for testing the pipeline without hardware.
//
//	go run ./cmd/devsim -url ws://127.0.0.1:8480/ws/device -token DEVTOKEN -in question.wav -out reply.wav
//	go run ./cmd/devsim ... -wake clip.wav      # send a wake-word clip instead and report the verdict
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"time"

	"github.com/gorilla/websocket"

	"otter-chan/server/internal/audio"
)

func main() {
	url := flag.String("url", "ws://127.0.0.1:8480/ws/device", "server websocket URL")
	token := flag.String("token", os.Getenv("OTTER_DEVICE_TOKEN"), "device token")
	in := flag.String("in", "", "16 kHz mono WAV to send as the utterance")
	wakeClip := flag.String("wake", "", "WAV to send as a wake-word clip")
	out := flag.String("out", "reply.wav", "where to write the spoken reply")
	wait := flag.Duration("wait", 90*time.Second, "how long to wait for the reply")
	flag.Parse()

	hdr := http.Header{"Authorization": {"Bearer " + *token}, "X-Otter-Name": {"devsim"}}
	ws, _, err := websocket.DefaultDialer.Dial(*url, hdr)
	if err != nil {
		log.Fatalf("dial: %v", err)
	}
	defer ws.Close()
	send := func(v map[string]any) {
		b, _ := json.Marshal(v)
		ws.WriteMessage(websocket.TextMessage, b)
	}
	send(map[string]any{"type": "hello", "name": "devsim", "fw": "sim", "rate": 16000, "mode": "standby"})

	var pcm []byte
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			mt, data, err := ws.ReadMessage()
			if err != nil {
				return
			}
			if mt == websocket.BinaryMessage && len(data) > 0 && data[0] == 0x01 {
				pcm = append(pcm, data[1:]...)
				continue
			}
			if mt == websocket.TextMessage {
				fmt.Println("<-", string(data))
				var m map[string]any
				json.Unmarshal(data, &m)
				switch m["type"] {
				case "say_end":
					if *in != "" {
						return
					}
				case "wake":
					fmt.Println("WAKE verdict: yes")
					return
				}
			}
		}
	}()

	loadPCM := func(path string) []byte {
		f, err := os.Open(path)
		if err != nil {
			log.Fatal(err)
		}
		defer f.Close()
		w, err := audio.ReadWAV(f)
		if err != nil {
			log.Fatal(err)
		}
		return audio.Bytes(audio.Resample(w.Mono(), w.Rate, 16000))
	}

	if *wakeClip != "" {
		clip := loadPCM(*wakeClip)
		ws.WriteMessage(websocket.BinaryMessage, append([]byte{0x02}, clip...))
		select {
		case <-done:
		case <-time.After(20 * time.Second):
			fmt.Println("WAKE verdict: no (timeout)")
		}
		return
	}
	if *in == "" {
		log.Fatal("-in or -wake required")
	}
	utt := loadPCM(*in)
	send(map[string]any{"type": "listen_start"})
	for i := 0; i < len(utt); i += 1024 {
		end := i + 1024
		if end > len(utt) {
			end = len(utt)
		}
		ws.WriteMessage(websocket.BinaryMessage, append([]byte{0x01}, utt[i:end]...))
	}
	send(map[string]any{"type": "listen_end", "reason": "silence"})
	select {
	case <-done:
	case <-time.After(*wait):
		fmt.Println("timeout waiting for reply")
	}
	if len(pcm) > 0 {
		os.WriteFile(*out, audio.WrapWAV(pcm, 16000), 0o644)
		fmt.Printf("reply audio: %.1fs -> %s\n", float64(len(pcm))/32000, *out)
	}
}
