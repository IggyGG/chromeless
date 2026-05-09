package server

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"regexp"
	"strings"
	"time"

	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

const (
	defaultGatewayBind    = ":8082"
	defaultGatewayTimeout = 60 * time.Second
	defaultGatewayPoll    = 500 * time.Millisecond
)

// GatewayConfig configures the REST session mint API consumed by Triform.
type GatewayConfig struct {
	Bind        string
	Namespace   string
	DefaultPool string
	ReadyWait   time.Duration
}

// SessionGateway exposes a tiny REST API over BrowserSession CRs:
// POST /v1/sessions, POST /v1/sessions/{name}/heartbeat, DELETE /v1/sessions/{name}.
type SessionGateway struct {
	Client client.Client
	Scheme *runtime.Scheme
	Config GatewayConfig
}

type mintRequest struct {
	TenantID           string `json:"tenant_id"`
	ElementID          string `json:"element_id"`
	PoolName           string `json:"pool_name"`
	Region             string `json:"region"`
	IdleTimeoutSeconds int32  `json:"idle_timeout_seconds"`
	SignalingSessionID string `json:"signaling_session_id"`
	SignalingURL       string `json:"signaling_url"`
	SignalingToken     string `json:"signaling_token"`
}

type sessionResponse struct {
	SignalingURL string `json:"signaling_url"`
	SessionID    string `json:"session_id"`
	PodName      string `json:"pod_name"`
	PodIP        string `json:"pod_ip"`
	Phase        string `json:"phase"`
}

func DefaultGatewayConfig() GatewayConfig {
	return GatewayConfig{
		Bind:      defaultGatewayBind,
		Namespace: "chromeless",
		ReadyWait: defaultGatewayTimeout,
	}
}

func (g *SessionGateway) Start(ctx context.Context) error {
	cfg := g.normalizedConfig()
	srv := &http.Server{
		Addr:              cfg.Bind,
		Handler:           g.Routes(),
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		<-ctx.Done()
		shutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutCtx)
	}()
	err := srv.ListenAndServe()
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

func (g *SessionGateway) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/sessions", g.handleSessions)
	mux.HandleFunc("/v1/sessions/", g.handleSessionByID)
	return mux
}

func (g *SessionGateway) normalizedConfig() GatewayConfig {
	cfg := g.Config
	def := DefaultGatewayConfig()
	if cfg.Bind == "" {
		cfg.Bind = def.Bind
	}
	if cfg.Namespace == "" {
		cfg.Namespace = def.Namespace
	}
	if cfg.ReadyWait == 0 {
		cfg.ReadyWait = def.ReadyWait
	}
	return cfg
}

func (g *SessionGateway) handleSessions(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req mintRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, fmt.Sprintf("decode request: %v", err), http.StatusBadRequest)
		return
	}
	if req.ElementID == "" {
		http.Error(w, "element_id is required", http.StatusBadRequest)
		return
	}

	cfg := g.normalizedConfig()
	name := sessionNameForElement(req.ElementID)
	sess := &cbv1.BrowserSession{}
	key := types.NamespacedName{Namespace: cfg.Namespace, Name: name}
	if err := g.Client.Get(r.Context(), key, sess); err != nil {
		if !apierrors.IsNotFound(err) {
			http.Error(w, fmt.Sprintf("get session: %v", err), http.StatusInternalServerError)
			return
		}
		sess = browserSessionFromMintRequest(req, cfg, name)
		if err := g.Client.Create(r.Context(), sess); err != nil {
			http.Error(w, fmt.Sprintf("create session: %v", err), http.StatusInternalServerError)
			return
		}
	} else if sess.DeletionTimestamp.IsZero() {
		desired := browserSessionFromMintRequest(req, cfg, name)
		sess.Spec = desired.Spec
		if sess.Annotations == nil {
			sess.Annotations = map[string]string{}
		}
		for k, v := range desired.Annotations {
			if v != "" {
				sess.Annotations[k] = v
			}
		}
		if err := g.Client.Update(r.Context(), sess); err != nil {
			http.Error(w, fmt.Sprintf("update session: %v", err), http.StatusInternalServerError)
			return
		}
	}

	ready, err := g.waitForReady(r.Context(), key, cfg.ReadyWait)
	if err != nil {
		http.Error(w, fmt.Sprintf("wait for ready: %v", err), http.StatusInternalServerError)
		return
	}
	status := http.StatusOK
	if ready.Status.Phase != cbv1.SessionReady {
		status = http.StatusAccepted
	}
	writeJSON(w, status, responseFromMintRequest(ready, name, req))
}

func (g *SessionGateway) handleSessionByID(w http.ResponseWriter, r *http.Request) {
	trimmed := strings.TrimPrefix(r.URL.Path, "/v1/sessions/")
	parts := strings.Split(strings.Trim(trimmed, "/"), "/")
	if len(parts) == 0 || parts[0] == "" {
		http.Error(w, "session id is required", http.StatusBadRequest)
		return
	}
	sessionID := parts[0]
	if len(parts) == 2 && parts[1] == "heartbeat" {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", http.MethodPost)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		g.handleHeartbeat(w, r, sessionID)
		return
	}
	if len(parts) == 1 && r.Method == http.MethodDelete {
		g.handleDelete(w, r, sessionID)
		return
	}
	http.NotFound(w, r)
}

func (g *SessionGateway) handleHeartbeat(w http.ResponseWriter, r *http.Request, sessionID string) {
	cfg := g.normalizedConfig()
	var sess cbv1.BrowserSession
	if err := g.Client.Get(r.Context(), types.NamespacedName{Namespace: cfg.Namespace, Name: sessionID}, &sess); err != nil {
		if apierrors.IsNotFound(err) {
			http.Error(w, "session not found", http.StatusNotFound)
			return
		}
		http.Error(w, fmt.Sprintf("get session: %v", err), http.StatusInternalServerError)
		return
	}
	now := metav1.NewTime(time.Now())
	sess.Status.LastActivityAt = &now
	if err := g.Client.Status().Update(r.Context(), &sess); err != nil {
		http.Error(w, fmt.Sprintf("update heartbeat: %v", err), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (g *SessionGateway) handleDelete(w http.ResponseWriter, r *http.Request, sessionID string) {
	cfg := g.normalizedConfig()
	var sess cbv1.BrowserSession
	if err := g.Client.Get(r.Context(), types.NamespacedName{Namespace: cfg.Namespace, Name: sessionID}, &sess); err != nil {
		if apierrors.IsNotFound(err) {
			writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
			return
		}
		http.Error(w, fmt.Sprintf("get session: %v", err), http.StatusInternalServerError)
		return
	}
	if err := g.Client.Delete(r.Context(), &sess); err != nil && !apierrors.IsNotFound(err) {
		http.Error(w, fmt.Sprintf("delete session: %v", err), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (g *SessionGateway) waitForReady(ctx context.Context, key types.NamespacedName, timeout time.Duration) (cbv1.BrowserSession, error) {
	logger := log.FromContext(ctx)
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	tick := time.NewTicker(defaultGatewayPoll)
	defer tick.Stop()

	var sess cbv1.BrowserSession
	for {
		if err := g.Client.Get(ctx, key, &sess); err != nil {
			if apierrors.IsNotFound(err) {
				select {
				case <-ctx.Done():
					return sess, ctx.Err()
				case <-deadline.C:
					logger.Info("session gateway returning before session cache observed create", "session", key)
					return sess, nil
				case <-tick.C:
					continue
				}
			}
			return sess, err
		}
		if sess.Status.Phase == cbv1.SessionReady {
			return sess, nil
		}
		select {
		case <-ctx.Done():
			return sess, ctx.Err()
		case <-deadline.C:
			logger.Info("session gateway returning before session ready", "session", key, "phase", sess.Status.Phase)
			return sess, nil
		case <-tick.C:
		}
	}
}

func browserSessionFromMintRequest(req mintRequest, cfg GatewayConfig, name string) *cbv1.BrowserSession {
	pool := req.PoolName
	if pool == "" {
		pool = cfg.DefaultPool
	}
	if pool == "" {
		pool = "default-pool"
	}
	idle := req.IdleTimeoutSeconds
	if idle <= 0 {
		idle = 600
	}
	brokerSessionID := req.SignalingSessionID
	if brokerSessionID == "" {
		brokerSessionID = name
	}
	annotations := map[string]string{
		cbv1.AnnotationBrokerSessionID:       brokerSessionID,
		cbv1.AnnotationBrowserSignalingURL:   req.SignalingURL,
		cbv1.AnnotationBrowserSignalingToken: req.SignalingToken,
	}
	return &cbv1.BrowserSession{
		ObjectMeta: metav1.ObjectMeta{
			Name:        name,
			Namespace:   cfg.Namespace,
			Annotations: annotations,
		},
		Spec: cbv1.BrowserSessionSpec{
			TenantID:           tenantForSpec(req.TenantID),
			PoolName:           pool,
			Region:             req.Region,
			IdleTimeoutSeconds: idle,
		},
	}
}

func tenantForSpec(tenant string) string {
	if tenant == "" {
		return cbv1.AnonymousTenant
	}
	return tenant
}

var invalidSessionNameChars = regexp.MustCompile(`[^a-z0-9-]+`)

func sessionNameForElement(elementID string) string {
	base := strings.ToLower(strings.TrimSpace(elementID))
	base = invalidSessionNameChars.ReplaceAllString(base, "-")
	base = strings.Trim(base, "-")
	if base == "" {
		base = "session"
	}
	if len(base) > 50 {
		base = strings.Trim(base[:50], "-")
	}
	return "tf-" + base
}

func responseFromSession(sess cbv1.BrowserSession) sessionResponse {
	resp := sessionResponse{
		SessionID: sess.Name,
		Phase:     string(sess.Status.Phase),
	}
	if sess.Status.Connection != nil {
		resp.SignalingURL = sess.Status.Connection.SignalingURL
		resp.PodName = sess.Status.Connection.PodName
		resp.PodIP = sess.Status.Connection.PodIP
	}
	// The gateway can update the desired browser signaling URL for an already
	// ready session before the reconciler has refreshed status. Return the
	// desired URL so Triform dials the owner pod that minted this session.
	if sess.Annotations[cbv1.AnnotationBrowserSignalingURL] != "" {
		resp.SignalingURL = sess.Annotations[cbv1.AnnotationBrowserSignalingURL]
	}
	return resp
}

func responseFromMintRequest(sess cbv1.BrowserSession, sessionName string, req mintRequest) sessionResponse {
	resp := responseFromSession(sess)
	if resp.SessionID == "" {
		resp.SessionID = sessionName
	}
	// The create/update has just accepted this desired URL. The informer cache
	// used by waitForReady may still return an already-ready object with stale
	// annotations, so preserve the caller's owner-pinned signaling URL here.
	if req.SignalingURL != "" {
		resp.SignalingURL = req.SignalingURL
	}
	return resp
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
