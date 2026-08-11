// Package main — tls.go
//
// Certificate provisioning. Two modes:
//
//   - Operator-supplied: CHROMELESS_TLS_CERT + CHROMELESS_TLS_KEY point at
//     PEMs and we simply load them.
//   - Self-signed: nothing supplied, so generate a P-256 pair into
//     CHROMELESS_TLS_DIR on first boot and reuse it forever after.
//
// The self-signed path exists because the alternative is worse. WebRTC needs a
// secure context for getUserMedia and the clipboard API, and a https:// page
// cannot dial a ws:// broker (mixed content is blocked outright) — so "just
// run it over http for local testing" does not actually work beyond the video
// element. Requiring the operator to produce a certificate before the stack
// runs at all would make the five-minute quickstart impossible, and pushing
// people toward `--ignore-certificate-errors` teaches a habit that is far more
// dangerous than one click-through on a localhost cert.
//
// Because the page and the WebSocket share one origin and one port, that
// click-through covers both. Splitting them across ports would cost a second,
// far more confusing trust prompt on a URL the user never typed.

package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"log/slog"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"time"
)

// certValidity is deliberately long. This is a locally-trusted-by-exception
// certificate for a single-operator deployment; a short lifetime would only
// mean a repeated browser warning with no security benefit, since nothing
// checks revocation for it.
const certValidity = 10 * 365 * 24 * time.Hour

// loadOrCreateCert returns a certificate for the TLS listener, generating and
// persisting a self-signed one if the operator supplied none.
func loadOrCreateCert(cfg *config, logger *slog.Logger) (tls.Certificate, error) {
	if cfg.certFile != "" {
		crt, err := tls.LoadX509KeyPair(cfg.certFile, cfg.keyFile)
		if err != nil {
			return tls.Certificate{}, fmt.Errorf("load supplied keypair: %w", err)
		}
		logger.Info("TLS: using supplied certificate",
			slog.String("cert", cfg.certFile), slog.String("key", cfg.keyFile))
		return crt, nil
	}

	certPath := filepath.Join(cfg.tlsDir, "cert.pem")
	keyPath := filepath.Join(cfg.tlsDir, "key.pem")

	// Reuse across restarts. Regenerating on every boot would re-prompt the
	// browser each time and, worse, invalidate a cert the operator may have
	// deliberately added to their trust store.
	if crt, err := tls.LoadX509KeyPair(certPath, keyPath); err == nil {
		logger.Info("TLS: reusing self-signed certificate", slog.String("dir", cfg.tlsDir))
		return crt, nil
	}

	if err := os.MkdirAll(cfg.tlsDir, 0o700); err != nil {
		return tls.Certificate{}, fmt.Errorf("create %s: %w", cfg.tlsDir, err)
	}

	certPEM, keyPEM, err := generateSelfSigned(cfg.tlsHosts)
	if err != nil {
		return tls.Certificate{}, err
	}

	// Key first and 0600: if the process dies between the two writes, the next
	// boot finds an incomplete pair, fails LoadX509KeyPair, and regenerates —
	// rather than leaving a world-readable key behind.
	if err := os.WriteFile(keyPath, keyPEM, 0o600); err != nil {
		return tls.Certificate{}, fmt.Errorf("write %s: %w", keyPath, err)
	}
	if err := os.WriteFile(certPath, certPEM, 0o644); err != nil {
		return tls.Certificate{}, fmt.Errorf("write %s: %w", certPath, err)
	}

	crt, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("parse generated keypair: %w", err)
	}
	logger.Warn("TLS: generated a self-signed certificate — your browser will "+
		"warn once on first visit; accepting it also covers the wss:// signaling "+
		"dial, since the page and the WebSocket share this origin",
		slog.String("dir", cfg.tlsDir))
	return crt, nil
}

// generateSelfSigned builds a P-256 self-signed leaf valid for localhost plus
// any extra hosts the operator named.
func generateSelfSigned(extraHosts []string) (certPEM, keyPEM []byte, err error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, fmt.Errorf("generate key: %w", err)
	}

	serialMax := new(big.Int).Lsh(big.NewInt(1), 128)
	serial, err := rand.Int(rand.Reader, serialMax)
	if err != nil {
		return nil, nil, fmt.Errorf("generate serial: %w", err)
	}

	tmpl := x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "chromeless", Organization: []string{"chromeless standalone"}},
		NotBefore:             time.Now().Add(-1 * time.Hour), // tolerate modest host clock skew
		NotAfter:              time.Now().Add(certValidity),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment | x509.KeyUsageCertSign,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		IsCA:                  true,
		DNSNames:              []string{"localhost"},
		IPAddresses:           []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")},
	}

	// Extra hosts let a split-host or LAN deployment get a usable cert without
	// switching to BYO. Anything that parses as an IP goes in IPAddresses —
	// browsers do NOT fall back to matching a DNS SAN against a literal IP, so
	// putting "192.168.1.5" in DNSNames produces a cert that silently fails to
	// validate for the URL the user actually typed.
	for _, h := range extraHosts {
		if ip := net.ParseIP(h); ip != nil {
			tmpl.IPAddresses = append(tmpl.IPAddresses, ip)
			continue
		}
		tmpl.DNSNames = append(tmpl.DNSNames, h)
	}

	der, err := x509.CreateCertificate(rand.Reader, &tmpl, &tmpl, &key.PublicKey, key)
	if err != nil {
		return nil, nil, fmt.Errorf("create certificate: %w", err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		return nil, nil, fmt.Errorf("marshal key: %w", err)
	}

	certPEM = pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyPEM = pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
	return certPEM, keyPEM, nil
}
