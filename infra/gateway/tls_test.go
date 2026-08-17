package main

import (
	"crypto/x509"
	"encoding/pem"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"testing"
)

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestGeneratesSelfSignedOnFirstBoot(t *testing.T) {
	dir := t.TempDir()
	cfg := &config{tlsDir: dir}

	crt, err := loadOrCreateCert(cfg, quietLogger())
	if err != nil {
		t.Fatalf("loadOrCreateCert: %v", err)
	}
	if len(crt.Certificate) == 0 {
		t.Fatal("no certificate produced")
	}
	for _, name := range []string{"cert.pem", "key.pem"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Errorf("%s not persisted: %v", name, err)
		}
	}
}

// Regenerating on every boot would re-prompt the browser each restart and
// invalidate a cert the operator may have added to their trust store.
func TestReusesCertAcrossRestarts(t *testing.T) {
	dir := t.TempDir()
	cfg := &config{tlsDir: dir}
	log := quietLogger()

	first, err := loadOrCreateCert(cfg, log)
	if err != nil {
		t.Fatalf("first: %v", err)
	}
	second, err := loadOrCreateCert(cfg, log)
	if err != nil {
		t.Fatalf("second: %v", err)
	}

	firstLeaf, err := x509.ParseCertificate(first.Certificate[0])
	if err != nil {
		t.Fatal(err)
	}
	secondLeaf, err := x509.ParseCertificate(second.Certificate[0])
	if err != nil {
		t.Fatal(err)
	}
	if firstLeaf.SerialNumber.Cmp(secondLeaf.SerialNumber) != 0 {
		t.Error("certificate was regenerated instead of reused")
	}
}

// The private key must never be group- or world-readable, even inside a
// container: the cert directory is a mounted volume the operator may inspect.
func TestPrivateKeyIsNotWorldReadable(t *testing.T) {
	dir := t.TempDir()
	if _, err := loadOrCreateCert(&config{tlsDir: dir}, quietLogger()); err != nil {
		t.Fatalf("loadOrCreateCert: %v", err)
	}
	info, err := os.Stat(filepath.Join(dir, "key.pem"))
	if err != nil {
		t.Fatal(err)
	}
	if perm := info.Mode().Perm(); perm != 0o600 {
		t.Errorf("key.pem mode = %o, want 600", perm)
	}
}

func TestSelfSignedCoversLocalhost(t *testing.T) {
	certPEM, _, err := generateSelfSigned(nil)
	if err != nil {
		t.Fatalf("generateSelfSigned: %v", err)
	}
	leaf := parseLeaf(t, certPEM)

	if err := leaf.VerifyHostname("localhost"); err != nil {
		t.Errorf("localhost not covered: %v", err)
	}
	for _, ip := range []string{"127.0.0.1", "::1"} {
		if err := leaf.VerifyHostname(ip); err != nil {
			t.Errorf("%s not covered: %v", ip, err)
		}
	}
}

// A literal IP must land in IPAddresses, not DNSNames. Browsers do not match a
// DNS SAN against an IP, so getting this wrong yields a cert that silently
// fails to validate for the URL the operator actually typed.
func TestExtraHostsSplitIPsFromNames(t *testing.T) {
	certPEM, _, err := generateSelfSigned([]string{"192.168.1.5", "browser.lan"})
	if err != nil {
		t.Fatalf("generateSelfSigned: %v", err)
	}
	leaf := parseLeaf(t, certPEM)

	if err := leaf.VerifyHostname("192.168.1.5"); err != nil {
		t.Errorf("IP SAN not honoured: %v", err)
	}
	if err := leaf.VerifyHostname("browser.lan"); err != nil {
		t.Errorf("DNS SAN not honoured: %v", err)
	}
	for _, got := range leaf.DNSNames {
		if got == "192.168.1.5" {
			t.Error("IP address was placed in DNSNames, where browsers ignore it")
		}
	}
	if !containsIP(leaf.IPAddresses, "192.168.1.5") {
		t.Error("192.168.1.5 missing from IPAddresses")
	}
}

// Cert generation must not depend on a pre-existing directory: the volume is
// empty on a first run.
func TestCreatesMissingCertDir(t *testing.T) {
	nested := filepath.Join(t.TempDir(), "does", "not", "exist")
	if _, err := loadOrCreateCert(&config{tlsDir: nested}, quietLogger()); err != nil {
		t.Fatalf("loadOrCreateCert: %v", err)
	}
	if _, err := os.Stat(filepath.Join(nested, "cert.pem")); err != nil {
		t.Errorf("cert not written into a created dir: %v", err)
	}
}

// A supplied cert must be used verbatim — silently substituting a self-signed
// one would serve an identity the operator did not choose.
func TestUsesSuppliedKeypair(t *testing.T) {
	dir := t.TempDir()
	certPEM, keyPEM, err := generateSelfSigned([]string{"supplied.example"})
	if err != nil {
		t.Fatal(err)
	}
	certPath := filepath.Join(dir, "supplied-cert.pem")
	keyPath := filepath.Join(dir, "supplied-key.pem")
	if err := os.WriteFile(certPath, certPEM, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyPath, keyPEM, 0o600); err != nil {
		t.Fatal(err)
	}

	crt, err := loadOrCreateCert(&config{
		certFile: certPath, keyFile: keyPath, tlsDir: filepath.Join(dir, "unused"),
	}, quietLogger())
	if err != nil {
		t.Fatalf("loadOrCreateCert: %v", err)
	}
	leaf, err := x509.ParseCertificate(crt.Certificate[0])
	if err != nil {
		t.Fatal(err)
	}
	if err := leaf.VerifyHostname("supplied.example"); err != nil {
		t.Errorf("supplied certificate was not used: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "unused", "cert.pem")); err == nil {
		t.Error("generated a self-signed cert despite one being supplied")
	}
}

// An unreadable or malformed supplied cert must fail loudly. Falling back to
// self-signed here would turn a deployment mistake into a silent downgrade.
func TestSuppliedKeypairFailureIsFatal(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.pem")
	if err := os.WriteFile(bad, []byte("not a certificate"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := loadOrCreateCert(&config{
		certFile: bad, keyFile: bad, tlsDir: dir,
	}, quietLogger()); err == nil {
		t.Fatal("expected an error for a malformed supplied keypair")
	}
}

func parseLeaf(t *testing.T, certPEM []byte) *x509.Certificate {
	t.Helper()
	block, _ := pem.Decode(certPEM)
	if block == nil {
		t.Fatal("no PEM block in generated certificate")
	}
	leaf, err := x509.ParseCertificate(block.Bytes)
	if err != nil {
		t.Fatalf("parse leaf: %v", err)
	}
	return leaf
}

func containsIP(ips []net.IP, want string) bool {
	target := net.ParseIP(want)
	for _, ip := range ips {
		if ip.Equal(target) {
			return true
		}
	}
	return false
}
