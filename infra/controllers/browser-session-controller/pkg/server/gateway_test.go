package server

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	clientgoscheme "k8s.io/client-go/kubernetes/scheme"
	"sigs.k8s.io/controller-runtime/pkg/client/fake"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

func gatewayScheme(t *testing.T) *runtime.Scheme {
	t.Helper()
	s := runtime.NewScheme()
	if err := clientgoscheme.AddToScheme(s); err != nil {
		t.Fatal(err)
	}
	if err := cbv1.AddToScheme(s); err != nil {
		t.Fatal(err)
	}
	return s
}

func TestGatewayCreateSessionStampsPatternCAnnotations(t *testing.T) {
	scheme := gatewayScheme(t)
	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()
	gw := &SessionGateway{
		Client: c,
		Scheme: scheme,
		Config: GatewayConfig{
			Namespace:   "cb",
			DefaultPool: "sw-pool",
			ReadyWait:   time.Nanosecond,
		},
	}

	body := mintRequest{
		TenantID:           "tenant-x",
		ElementID:          "11111111-2222-3333-4444-555555555555",
		IdleTimeoutSeconds: 300,
		SignalingSessionID: "cb:11111111-2222-3333-4444-555555555555",
		SignalingURL:       "ws://triform.triform-wtf.svc.cluster.local:3000/api/webrtc/signaling",
		SignalingToken:     "browser-jwt",
	}
	payload, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	req := httptest.NewRequest(http.MethodPost, "/v1/sessions", bytes.NewReader(payload))
	rec := httptest.NewRecorder()

	gw.Routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusAccepted {
		t.Fatalf("status = %d body=%s", rec.Code, rec.Body.String())
	}
	var resp sessionResponse
	if err := json.NewDecoder(rec.Body).Decode(&resp); err != nil {
		t.Fatal(err)
	}
	if resp.SessionID != "tf-11111111-2222-3333-4444-555555555555" {
		t.Fatalf("session_id = %q", resp.SessionID)
	}

	var sess cbv1.BrowserSession
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: resp.SessionID}, &sess); err != nil {
		t.Fatal(err)
	}
	if sess.Spec.PoolName != "sw-pool" {
		t.Fatalf("poolName = %q", sess.Spec.PoolName)
	}
	if sess.Spec.IdleTimeoutSeconds != 300 {
		t.Fatalf("idleTimeoutSeconds = %d", sess.Spec.IdleTimeoutSeconds)
	}
	if sess.Annotations[cbv1.AnnotationBrokerSessionID] != body.SignalingSessionID {
		t.Fatalf("broker session annotation = %q", sess.Annotations[cbv1.AnnotationBrokerSessionID])
	}
	if sess.Annotations[cbv1.AnnotationBrowserSignalingURL] != body.SignalingURL {
		t.Fatalf("signaling url annotation = %q", sess.Annotations[cbv1.AnnotationBrowserSignalingURL])
	}
	if sess.Annotations[cbv1.AnnotationBrowserSignalingToken] != body.SignalingToken {
		t.Fatalf("signaling token annotation = %q", sess.Annotations[cbv1.AnnotationBrowserSignalingToken])
	}
}

func TestResponseFromSessionPrefersDesiredSignalingURL(t *testing.T) {
	sess := cbv1.BrowserSession{
		ObjectMeta: metav1.ObjectMeta{
			Name: "tf-session",
			Annotations: map[string]string{
				cbv1.AnnotationBrowserSignalingURL: "ws://owner-pod.example/api/webrtc/signaling",
			},
		},
		Status: cbv1.BrowserSessionStatus{
			Phase: cbv1.SessionReady,
			Connection: &cbv1.SessionConnection{
				PodName:      "sw-pool-0",
				PodIP:        "10.244.0.10",
				SignalingURL: "ws://stale-pod.example/api/webrtc/signaling",
			},
		},
	}

	resp := responseFromSession(sess)

	if resp.SignalingURL != sess.Annotations[cbv1.AnnotationBrowserSignalingURL] {
		t.Fatalf("signaling_url = %q", resp.SignalingURL)
	}
	if resp.PodName != "sw-pool-0" {
		t.Fatalf("pod_name = %q", resp.PodName)
	}
	if resp.PodIP != "10.244.0.10" {
		t.Fatalf("pod_ip = %q", resp.PodIP)
	}
}

func TestResponseFromMintRequestPreservesRequestedSignalingURLAcrossCacheLag(t *testing.T) {
	sess := cbv1.BrowserSession{
		ObjectMeta: metav1.ObjectMeta{
			Name: "tf-session",
			Annotations: map[string]string{
				cbv1.AnnotationBrowserSignalingURL: "ws://old-owner.example/api/webrtc/signaling",
			},
		},
		Status: cbv1.BrowserSessionStatus{
			Phase: cbv1.SessionReady,
			Connection: &cbv1.SessionConnection{
				PodName:      "sw-pool-0",
				PodIP:        "10.244.0.10",
				SignalingURL: "ws://old-status.example/api/webrtc/signaling",
			},
		},
	}
	req := mintRequest{
		SignalingURL: "ws://new-owner.example/api/webrtc/signaling",
	}

	resp := responseFromMintRequest(sess, "tf-session", req)

	if resp.SignalingURL != req.SignalingURL {
		t.Fatalf("signaling_url = %q", resp.SignalingURL)
	}
	if resp.SessionID != "tf-session" {
		t.Fatalf("session_id = %q", resp.SessionID)
	}
	if resp.PodName != "sw-pool-0" {
		t.Fatalf("pod_name = %q", resp.PodName)
	}
}

func TestGatewayHeartbeatUpdatesLastActivity(t *testing.T) {
	scheme := gatewayScheme(t)
	old := metav1.NewTime(time.Now().Add(-time.Hour))
	sess := &cbv1.BrowserSession{
		ObjectMeta: metav1.ObjectMeta{Name: "tf-session", Namespace: "cb"},
		Spec: cbv1.BrowserSessionSpec{
			TenantID:           "tenant-x",
			PoolName:           "default-pool",
			IdleTimeoutSeconds: 600,
		},
		Status: cbv1.BrowserSessionStatus{
			Phase:          cbv1.SessionReady,
			LastActivityAt: &old,
		},
	}
	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(sess).
		WithStatusSubresource(&cbv1.BrowserSession{}).
		Build()
	gw := &SessionGateway{
		Client: c,
		Scheme: scheme,
		Config: GatewayConfig{Namespace: "cb", ReadyWait: time.Nanosecond},
	}

	req := httptest.NewRequest(http.MethodPost, "/v1/sessions/tf-session/heartbeat", nil)
	rec := httptest.NewRecorder()
	gw.Routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d body=%s", rec.Code, rec.Body.String())
	}
	var got cbv1.BrowserSession
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "tf-session"}, &got); err != nil {
		t.Fatal(err)
	}
	if got.Status.LastActivityAt == nil || !got.Status.LastActivityAt.After(old.Time) {
		t.Fatalf("LastActivityAt was not advanced: old=%v got=%v", old.Time, got.Status.LastActivityAt)
	}
}
