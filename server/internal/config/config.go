// Package config loads server settings from the environment (.env-style files are the deployer's job).
package config

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Config struct {
	Host string
	Port int

	// Tokens maps bearer token -> identity name. "device" is the robot.
	Tokens map[string]string

	LLMBaseURL   string
	LLMModel     string
	LLMReasoning string
	LLMAPIKey    string

	STTURL string // parakeet-server base URL, e.g. http://stt:8080

	TTSEngine  string  // piper (fast, default) | vox (Audio8 clone via otter-vox)
	PiperBin   string  // piper binary
	PiperModel string  // voice .onnx
	PiperSpeed float64 // length_scale; 1.0 = normal

	VoxBin   string // otter-vox binary
	VoxVoice string
	VoxArgs  []string // extra args (e.g. --cpu, --cpu-threads 8, --model-dir ...)

	Name          string
	WakePhrases   []string
	WakeThreshold int // 0..100 similarity
	Followup      bool
	DataDir       string
	AudioRate     int
}

func env(key, def string) string {
	if v := strings.TrimSpace(os.Getenv(key)); v != "" {
		return v
	}
	return def
}

func envBool(key string, def bool) bool {
	v := strings.ToLower(env(key, ""))
	if v == "" {
		return def
	}
	return v == "1" || v == "true" || v == "yes" || v == "on"
}

func envFloat(key string, def float64) float64 {
	if f, err := strconv.ParseFloat(env(key, ""), 64); err == nil {
		return f
	}
	return def
}

func envInt(key string, def int) int {
	if n, err := strconv.Atoi(env(key, "")); err == nil {
		return n
	}
	return def
}

// ResolveMetaKey: META_MODEL_API_KEY -> META_API_KEY -> Muse Code plan login (~/.config/muse/auth.json).
func ResolveMetaKey() string {
	for _, k := range []string{"META_MODEL_API_KEY", "META_API_KEY"} {
		if v := env(k, ""); v != "" {
			return v
		}
	}
	base := os.Getenv("XDG_CONFIG_HOME")
	if base == "" {
		home, _ := os.UserHomeDir()
		base = filepath.Join(home, ".config")
	}
	for _, p := range []string{env("MUSE_AUTH_PATH", ""), filepath.Join(base, "muse", "auth.json")} {
		if p == "" {
			continue
		}
		raw, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		var doc struct {
			Providers map[string]struct {
				APIKey string `json:"api_key"`
			} `json:"providers"`
		}
		if json.Unmarshal(raw, &doc) == nil {
			if m, ok := doc.Providers["meta"]; ok && m.APIKey != "" {
				return m.APIKey
			}
		}
	}
	return ""
}

func parseTokens(spec string) map[string]string {
	out := map[string]string{}
	for _, part := range strings.Split(spec, ",") {
		part = strings.TrimSpace(part)
		name, tok, ok := strings.Cut(part, ":")
		if !ok || name == "" || tok == "" {
			continue
		}
		out[strings.TrimSpace(tok)] = strings.TrimSpace(name)
	}
	return out
}

func splitList(s string) []string {
	var out []string
	for _, p := range strings.Split(s, ",") {
		if p = strings.TrimSpace(strings.ToLower(p)); p != "" {
			out = append(out, p)
		}
	}
	return out
}

func Load() (*Config, error) {
	c := &Config{
		Host:          env("OTTER_HOST", "0.0.0.0"),
		Port:          envInt("OTTER_PORT", 8480),
		Tokens:        parseTokens(env("OTTER_TOKENS", "")),
		LLMBaseURL:    strings.TrimRight(env("OTTER_LLM_BASE_URL", "https://api.meta.ai/v1"), "/"),
		LLMModel:      env("OTTER_LLM_MODEL", "muse-spark-1.3-contributor"),
		LLMReasoning:  env("OTTER_LLM_REASONING", "minimal"),
		LLMAPIKey:     ResolveMetaKey(),
		STTURL:        strings.TrimRight(env("OTTER_STT_URL", "http://127.0.0.1:8080"), "/"),
		TTSEngine:     env("OTTER_TTS_ENGINE", "piper"),
		PiperBin:      env("OTTER_PIPER_BIN", "piper"),
		PiperModel:    env("OTTER_PIPER_MODEL", "/opt/piper/voices/en_GB-northern_english_male-medium.onnx"),
		PiperSpeed:    envFloat("OTTER_PIPER_SPEED", 1.0),
		VoxBin:        env("OTTER_VOX_BIN", "otter-vox"),
		VoxVoice:      env("OTTER_VOX_VOICE", "audio8-en-calm"),
		VoxArgs:       strings.Fields(env("OTTER_VOX_ARGS", "--cpu")),
		Name:          env("OTTER_NAME", "Tarquin"),
		WakePhrases:   splitList(env("OTTER_WAKE_PHRASES", "tarquin,hey tarquin,ok tarquin")),
		WakeThreshold: envInt("OTTER_WAKE_THRESHOLD", 72),
		Followup:      envBool("OTTER_FOLLOWUP", true),
		DataDir:       env("OTTER_DATA_DIR", "./data"),
		AudioRate:     16000,
	}
	if len(c.Tokens) == 0 {
		return nil, fmt.Errorf("OTTER_TOKENS is empty: set at least device:<token>")
	}
	if err := os.MkdirAll(c.DataDir, 0o755); err != nil {
		return nil, err
	}
	return c, nil
}
