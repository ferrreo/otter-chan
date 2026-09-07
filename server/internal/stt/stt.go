// Package stt talks to parakeet-server (parakeet.cpp), an OpenAI-style /v1/audio/transcriptions endpoint.
package stt

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"strings"
	"time"

	"otter-chan/server/internal/audio"
)

type Client struct {
	BaseURL string
	Rate    int
	HTTP    *http.Client
}

func New(baseURL string, rate int) *Client {
	return &Client{BaseURL: baseURL, Rate: rate, HTTP: &http.Client{Timeout: 60 * time.Second}}
}

// Transcribe sends 16 kHz s16le mono PCM and returns the transcript.
func (c *Client) Transcribe(ctx context.Context, pcm []byte) (string, error) {
	if len(pcm) < c.Rate*2/4 { // < 250 ms
		return "", nil
	}
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	fw, _ := mw.CreateFormFile("file", "clip.wav")
	fw.Write(audio.WrapWAV(pcm, c.Rate))
	mw.WriteField("response_format", "json")
	mw.WriteField("model", "parakeet")
	mw.Close()
	req, err := http.NewRequestWithContext(ctx, "POST", c.BaseURL+"/v1/audio/transcriptions", &body)
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", mw.FormDataContentType())
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != 200 {
		return "", fmt.Errorf("stt http %d: %s", resp.StatusCode, strings.TrimSpace(string(raw)))
	}
	var out struct {
		Text string `json:"text"`
	}
	if err := json.Unmarshal(raw, &out); err != nil {
		return strings.TrimSpace(string(raw)), nil
	}
	return strings.TrimSpace(out.Text), nil
}

func (c *Client) Healthy(ctx context.Context) bool {
	ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	for _, p := range []string{"/health", "/v1/models", "/"} {
		req, _ := http.NewRequestWithContext(ctx, "GET", c.BaseURL+p, nil)
		if resp, err := c.HTTP.Do(req); err == nil {
			resp.Body.Close()
			if resp.StatusCode < 500 {
				return true
			}
		}
	}
	return false
}
