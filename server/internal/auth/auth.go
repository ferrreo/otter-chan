// Package auth resolves bearer tokens to identities.
package auth

import (
	"net/http"
	"strings"
)

type Resolver struct{ Tokens map[string]string }

// Identity returns the name bound to the request's bearer token (or ?token=), or "".
func (r Resolver) Identity(req *http.Request) string {
	h := req.Header.Get("Authorization")
	if strings.HasPrefix(strings.ToLower(h), "bearer ") {
		if id, ok := r.Tokens[strings.TrimSpace(h[7:])]; ok {
			return id
		}
	}
	if t := req.URL.Query().Get("token"); t != "" {
		return r.Tokens[t]
	}
	return ""
}

// Middleware rejects unauthenticated requests and stores the identity in the header X-Otter-Identity.
func (r Resolver) Middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
		id := r.Identity(req)
		if id == "" {
			w.Header().Set("WWW-Authenticate", "Bearer")
			http.Error(w, `{"error":"invalid or missing bearer token"}`, http.StatusUnauthorized)
			return
		}
		req.Header.Set("X-Otter-Identity", id)
		next.ServeHTTP(w, req)
	})
}
