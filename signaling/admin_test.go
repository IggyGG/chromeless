package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// resetAdminForTest swaps in a fresh keypair so each test runs with
// known signers. Returns the private half for signing.
func resetAdminForTest(t *testing.T) ed25519.PrivateKey {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatalf("keygen: %v", err)
	}
	prev := globalAdmin
	globalAdmin = adminConfig{enabled: true, pubKey: pub}
	t.Cleanup(func() { globalAdmin = prev })
	return priv
}

func adminToken(priv ed25519.PrivateKey, c Claims) string {
	if c.Role == "" {
		c.Role = "admin"
	}
	return signToken(priv, c)
}

func TestVerifyAdminToken_HappyPath(t *testing.T) {
	priv := resetAdminForTest(t)
	freezeTime(t, 1_700_000_000)
	tok := adminToken(priv, Claims{Sub: "ops", Role: "admin", Exp: 1_700_001_000})
	if _, err := verifyAdminToken(tok); err != nil {
		t.Fatalf("err: %v", err)
	}
}

func TestVerifyAdminToken_RejectsNonAdminRole(t *testing.T) {
	priv := resetAdminForTest(t)
	freezeTime(t, 1_700_000_000)
	// A token with role:client signed by the admin key is still
	// rejected — the admin endpoint requires explicit role:admin.
	tok := adminToken(priv, Claims{Sub: "ops", Role: "client", Exp: 1_700_001_000})
	_, err := verifyAdminToken(tok)
	if err == nil {
		t.Fatal("client-role token should be rejected")
	}
	if !strings.Contains(err.Error(), "admin role required") {
		t.Errorf("unexpected error: %v", err)
	}
}

func TestVerifyAdminToken_RejectsExpired(t *testing.T) {
	priv := resetAdminForTest(t)
	freezeTime(t, 1_700_000_000)
	tok := adminToken(priv, Claims{Sub: "ops", Role: "admin", Exp: 1_699_999_000})
	if _, err := verifyAdminToken(tok); err == nil {
		t.Fatal("expired admin token should be rejected")
	}
}

func TestVerifyAdminToken_RejectsBadSignature(t *testing.T) {
	resetAdminForTest(t)
	freezeTime(t, 1_700_000_000)
	_, otherPriv, _ := ed25519.GenerateKey(rand.Reader)
	tok := adminToken(otherPriv, Claims{Sub: "ops", Role: "admin", Exp: 1_700_001_000})
	if _, err := verifyAdminToken(tok); err == nil {
		t.Fatal("token signed by other key should be rejected")
	}
}

func TestVerifyAdminToken_DisabledWhenNoPubkey(t *testing.T) {
	prev := globalAdmin
	globalAdmin = adminConfig{enabled: false}
	t.Cleanup(func() { globalAdmin = prev })
	if _, err := verifyAdminToken("anything"); err == nil {
		t.Fatal("disabled admin should refuse all tokens")
	}
}

// ---------- /admin/revoke handler ----------

func TestAdminRevokeHandler_TenantBan(t *testing.T) {
	priv := resetAdminForTest(t)
	freezeTime(t, 1_700_000_000)
	deny := NewStaticDenylist()
	h := adminRevokeHandler(deny, quietLogger())

	tok := adminToken(priv, Claims{Sub: "ops", Role: "admin", Exp: 1_700_001_000})
	body := `{"tenant":"alice","reason":"compromised"}`
	r := httptest.NewRequest(http.MethodPost, "/admin/revoke", strings.NewReader(body))
	r.Header.Set("Authorization", "Bearer "+tok)
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusOK {
		t.Fatalf("status: got %d, want 200; body=%q", w.Code, w.Body.String())
	}
	if !strings.Contains(w.Body.String(), `"scope":"tenant"`) {
		t.Errorf("body should report scope:tenant: %q", w.Body.String())
	}
	tenants, _ := deny.Snapshot()
	if len(tenants) != 1 || tenants[0] != "alice" {
		t.Errorf("denylist did not capture alice: tenants=%v", tenants)
	}
}

func TestAdminRevokeHandler_JTIBan(t *testing.T) {
	priv := resetAdminForTest(t)
	freezeTime(t, 1_700_000_000)
	deny := NewStaticDenylist()
	h := adminRevokeHandler(deny, quietLogger())

	tok := adminToken(priv, Claims{Sub: "ops", Role: "admin", Exp: 1_700_001_000})
	body := `{"tenant":"alice","jti":"abc123","reason":"stolen"}`
	r := httptest.NewRequest(http.MethodPost, "/admin/revoke", strings.NewReader(body))
	r.Header.Set("Authorization", "Bearer "+tok)
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusOK {
		t.Fatalf("status: got %d, body=%q", w.Code, w.Body.String())
	}
	if !strings.Contains(w.Body.String(), `"scope":"jti"`) {
		t.Errorf("body should report scope:jti: %q", w.Body.String())
	}
	_, jtis := deny.Snapshot()
	want := "alice:abc123"
	found := false
	for _, j := range jtis {
		if j == want {
			found = true
		}
	}
	if !found {
		t.Errorf("denylist did not capture %q: jtis=%v", want, jtis)
	}
}

func TestAdminRevokeHandler_RejectsNonPOST(t *testing.T) {
	resetAdminForTest(t)
	deny := NewStaticDenylist()
	h := adminRevokeHandler(deny, quietLogger())
	r := httptest.NewRequest(http.MethodGet, "/admin/revoke", nil)
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("got %d, want 405", w.Code)
	}
}

func TestAdminRevokeHandler_RejectsMissingToken(t *testing.T) {
	resetAdminForTest(t)
	deny := NewStaticDenylist()
	h := adminRevokeHandler(deny, quietLogger())
	r := httptest.NewRequest(http.MethodPost, "/admin/revoke", strings.NewReader(`{"tenant":"x"}`))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("got %d, want 401", w.Code)
	}
}

func TestAdminRevokeHandler_RejectsBadToken(t *testing.T) {
	resetAdminForTest(t)
	freezeTime(t, 1_700_000_000)
	deny := NewStaticDenylist()
	h := adminRevokeHandler(deny, quietLogger())

	// Sign with a non-admin keypair.
	_, otherPriv, _ := ed25519.GenerateKey(rand.Reader)
	tok := adminToken(otherPriv, Claims{Sub: "ops", Role: "admin", Exp: 1_700_001_000})
	r := httptest.NewRequest(http.MethodPost, "/admin/revoke", strings.NewReader(`{"tenant":"x"}`))
	r.Header.Set("Authorization", "Bearer "+tok)
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("got %d, want 401", w.Code)
	}
}

func TestAdminRevokeHandler_RejectsMissingTenant(t *testing.T) {
	priv := resetAdminForTest(t)
	freezeTime(t, 1_700_000_000)
	deny := NewStaticDenylist()
	h := adminRevokeHandler(deny, quietLogger())

	tok := adminToken(priv, Claims{Sub: "ops", Role: "admin", Exp: 1_700_001_000})
	r := httptest.NewRequest(http.MethodPost, "/admin/revoke", strings.NewReader(`{"jti":"abc"}`))
	r.Header.Set("Authorization", "Bearer "+tok)
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("got %d, want 400", w.Code)
	}
}
