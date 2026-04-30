package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"reflect"
	"testing"
)

func envFromMap(m map[string]string) func(string) string {
	return func(k string) string { return m[k] }
}

func TestBuildICEConfig(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name string
		env  map[string]string
		want iceConfig
	}{
		{
			name: "stun-only default",
			env:  map[string]string{},
			want: iceConfig{IceServers: []iceServer{
				{URLs: defaultStunURLs},
			}},
		},
		{
			name: "stun override",
			env:  map[string]string{"STUN_URLS": "stun:stun.example.com:3478"},
			want: iceConfig{IceServers: []iceServer{
				{URLs: []string{"stun:stun.example.com:3478"}},
			}},
		},
		{
			name: "stun multi + whitespace",
			env:  map[string]string{"STUN_URLS": " stun:a.example.com:3478 , stun:b.example.com:3478 "},
			want: iceConfig{IceServers: []iceServer{
				{URLs: []string{"stun:a.example.com:3478", "stun:b.example.com:3478"}},
			}},
		},
		{
			name: "stun + turn",
			env: map[string]string{
				"TURN_URLS": "turn:turn.example.com:3478?transport=udp",
				"TURN_USER": "alice",
				"TURN_PASS": "s3cret",
			},
			want: iceConfig{IceServers: []iceServer{
				{URLs: defaultStunURLs},
				{
					URLs:       []string{"turn:turn.example.com:3478?transport=udp"},
					Username:   "alice",
					Credential: "s3cret",
				},
			}},
		},
		{
			name: "turn without user/pass omits credentials",
			env:  map[string]string{"TURN_URLS": "turn:turn.example.com:3478"},
			want: iceConfig{IceServers: []iceServer{
				{URLs: defaultStunURLs},
				{URLs: []string{"turn:turn.example.com:3478"}},
			}},
		},
		{
			name: "empty TURN_URLS is treated as unset",
			env:  map[string]string{"TURN_URLS": "  "},
			want: iceConfig{IceServers: []iceServer{
				{URLs: defaultStunURLs},
			}},
		},
		{
			name: "stun + turn with multiple URLs each",
			env: map[string]string{
				"STUN_URLS": "stun:a.example.com:3478,stun:b.example.com:3478",
				"TURN_URLS": "turn:t1.example.com:3478,turns:t1.example.com:5349",
				"TURN_USER": "u",
				"TURN_PASS": "p",
			},
			want: iceConfig{IceServers: []iceServer{
				{URLs: []string{"stun:a.example.com:3478", "stun:b.example.com:3478"}},
				{
					URLs:       []string{"turn:t1.example.com:3478", "turns:t1.example.com:5349"},
					Username:   "u",
					Credential: "p",
				},
			}},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := buildICEConfig(envFromMap(tc.env))
			if !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("got=%+v\nwant=%+v", got, tc.want)
			}
		})
	}
}

func TestTurnHandler(t *testing.T) {
	t.Setenv("STUN_URLS", "stun:stun.example.com:3478")
	t.Setenv("TURN_URLS", "turn:turn.example.com:3478")
	t.Setenv("TURN_USER", "alice")
	t.Setenv("TURN_PASS", "s3cret")

	req := httptest.NewRequest(http.MethodGet, "/turn-credentials", nil)
	rec := httptest.NewRecorder()
	turnHandler(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d, want 200", rec.Code)
	}
	if ct := rec.Header().Get("Content-Type"); ct != "application/json" {
		t.Fatalf("content-type=%q, want application/json", ct)
	}
	if rec.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("missing no-store cache header")
	}
	if rec.Header().Get("Access-Control-Allow-Origin") != "*" {
		t.Fatalf("missing CORS header")
	}

	var got iceConfig
	if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
		t.Fatalf("decode body: %v\nbody=%s", err, rec.Body.String())
	}
	if len(got.IceServers) != 2 {
		t.Fatalf("got %d iceServers, want 2: %+v", len(got.IceServers), got)
	}
	if got.IceServers[0].URLs[0] != "stun:stun.example.com:3478" {
		t.Fatalf("stun url mismatch: %+v", got.IceServers[0])
	}
	if got.IceServers[1].Username != "alice" || got.IceServers[1].Credential != "s3cret" {
		t.Fatalf("turn credentials mismatch: %+v", got.IceServers[1])
	}
}
