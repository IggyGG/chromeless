// Package server — admission_test.go
//
// Quick checks for the admission webhook decision logic.

package server

import (
	"context"
	"encoding/json"
	"testing"

	admissionv1 "k8s.io/api/admission/v1"
	authnv1 "k8s.io/api/authentication/v1"
	"k8s.io/apimachinery/pkg/runtime"
	runtimes "k8s.io/apimachinery/pkg/runtime/serializer/json"
	"k8s.io/apimachinery/pkg/types"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

func encode(t *testing.T, obj *cbv1.BrowserSession) []byte {
	t.Helper()
	scheme := runtime.NewScheme()
	if err := cbv1.AddToScheme(scheme); err != nil {
		t.Fatal(err)
	}
	codec := runtimes.NewSerializerWithOptions(
		runtimes.DefaultMetaFactory, scheme, scheme,
		runtimes.SerializerOptions{Yaml: false, Pretty: false, Strict: false},
	)
	out, err := runtime.Encode(codec, obj)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func TestAdmission_AllowsAnonymousWithEmptyTenant(t *testing.T) {
	sess := &cbv1.BrowserSession{}
	sess.Name = "s"
	sess.Namespace = "cb"
	raw, _ := json.Marshal(sess)
	req := &admissionv1.AdmissionRequest{
		UID:      types.UID("u1"),
		Object:   runtime.RawExtension{Raw: raw},
		UserInfo: authnv1.UserInfo{Username: "system:anonymous"},
		Operation: admissionv1.Create,
	}
	resp := ValidateBrowserSession(context.Background(), nil, req)
	if !resp.Allowed {
		t.Fatalf("expected allow; got: %+v", resp.Result)
	}
}

func TestAdmission_DeniesCrossTenant(t *testing.T) {
	sess := &cbv1.BrowserSession{
		Spec: cbv1.BrowserSessionSpec{TenantID: "tenant-A"},
	}
	sess.Name = "s"
	sess.Namespace = "cb"
	raw, _ := json.Marshal(sess)
	req := &admissionv1.AdmissionRequest{
		UID:    types.UID("u2"),
		Object: runtime.RawExtension{Raw: raw},
		UserInfo: authnv1.UserInfo{
			Username: "tenant:tenant-B",
		},
		Operation: admissionv1.Create,
	}
	resp := ValidateBrowserSession(context.Background(), nil, req)
	if resp.Allowed {
		t.Fatalf("expected deny; got allow")
	}
	if resp.Result == nil || resp.Result.Code != 403 {
		t.Fatalf("expected 403; got %+v", resp.Result)
	}
}

func TestAdmission_AllowsMatchingTenant(t *testing.T) {
	sess := &cbv1.BrowserSession{
		Spec: cbv1.BrowserSessionSpec{TenantID: "tenant-A"},
	}
	sess.Name = "s"
	sess.Namespace = "cb"
	raw, _ := json.Marshal(sess)
	req := &admissionv1.AdmissionRequest{
		UID:    types.UID("u3"),
		Object: runtime.RawExtension{Raw: raw},
		UserInfo: authnv1.UserInfo{
			Username: "tenant:tenant-A",
		},
		Operation: admissionv1.Create,
	}
	resp := ValidateBrowserSession(context.Background(), nil, req)
	if !resp.Allowed {
		t.Fatalf("expected allow; got %+v", resp.Result)
	}
}
