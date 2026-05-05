// Package server — admission.go
//
// Validating admission webhook for BrowserSession (T71). Phase 3
// multi-tenancy enforcement: a BrowserSession.spec.tenantID must
// match the requester's authenticated tenant claim.
//
// Today this is a *scaffold*. A real deployment needs:
//   - TLS cert (cert-manager.io / kube's CertController).
//   - ValidatingWebhookConfiguration pointing at the controller
//     Service:443/validate-chromeless-browsersession.
//   - The signaling-server's auth (T48) extended to issue per-tenant
//     ServiceAccount tokens that the K8s API server can verify and
//     present to this webhook in admission.Request.UserInfo.
//
// We ship the handler so the webhook config can land alongside it
// and the integration is one TLS-cert away from working.

package server

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"

	admissionv1 "k8s.io/api/admission/v1"
	authnv1 "k8s.io/api/authentication/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

// TenantClaimHeader is the request header the API server forwards
// from the OIDC/serviceaccount token after auth. Phase 3: the
// signaling auth (T48) issues tokens whose `tenant` claim lands here
// via a custom auth proxy. Until then we read the K8s SA name as a
// stand-in. See docs in the controller README.
const TenantClaimHeader = "X-Cb-Tenant"

// ValidateBrowserSession is the admission handler. It runs against
// CREATE/UPDATE on BrowserSession.
func ValidateBrowserSession(_ context.Context, scheme *runtime.Scheme, req *admissionv1.AdmissionRequest) *admissionv1.AdmissionResponse {
	resp := &admissionv1.AdmissionResponse{
		UID:     req.UID,
		Allowed: true,
	}

	// Decode the new object.
	var sess cbv1.BrowserSession
	if err := json.Unmarshal(req.Object.Raw, &sess); err != nil {
		return deny(resp, fmt.Sprintf("decode: %v", err))
	}

	// Phase 3 contract: tenantID must match the requester's tenant
	// claim. For UPDATE, the tenantID must not change.
	requesterTenant := tenantFrom(&req.UserInfo)
	if requesterTenant == "" {
		// Auth not yet wired (auth-disabled dev deploys). Allow but
		// stamp the request so the controller knows it's anonymous.
		if sess.Spec.TenantID != "" && sess.Spec.TenantID != cbv1.AnonymousTenant {
			return deny(resp, "spec.tenantID must be empty when caller is unauthenticated")
		}
		return resp
	}

	if sess.Spec.TenantID == "" {
		// Allow and let the mutating webhook (future) fill it in. For
		// now reject so misuse is obvious.
		return deny(resp, "spec.tenantID is required when caller is authenticated")
	}
	if sess.Spec.TenantID != requesterTenant {
		return deny(resp, fmt.Sprintf("tenant claim %q does not match spec.tenantID %q",
			requesterTenant, sess.Spec.TenantID))
	}

	// On UPDATE, ensure tenantID isn't being changed.
	if req.Operation == admissionv1.Update && len(req.OldObject.Raw) > 0 {
		var old cbv1.BrowserSession
		if err := json.Unmarshal(req.OldObject.Raw, &old); err == nil {
			if old.Spec.TenantID != "" && old.Spec.TenantID != sess.Spec.TenantID {
				return deny(resp, "spec.tenantID is immutable")
			}
		}
	}

	return resp
}

// tenantFrom extracts the requester's tenant from the K8s
// authentication.UserInfo. We look in (priority order):
//
//	1. user.Extra["tenant"] — when an auth proxy has added the claim
//	2. user.Username if it has a "tenant:..." prefix (dev convention)
//
// Returns "" when no claim is present (caller is anonymous).
func tenantFrom(u *authnv1.UserInfo) string {
	if u == nil {
		return ""
	}
	if vals, ok := u.Extra["tenant"]; ok && len(vals) > 0 {
		return string(vals[0])
	}
	const prefix = "tenant:"
	if len(u.Username) > len(prefix) && u.Username[:len(prefix)] == prefix {
		return u.Username[len(prefix):]
	}
	return ""
}

func deny(resp *admissionv1.AdmissionResponse, msg string) *admissionv1.AdmissionResponse {
	resp.Allowed = false
	resp.Result = &metav1.Status{
		Status:  metav1.StatusFailure,
		Message: msg,
		Reason:  metav1.StatusReasonForbidden,
		Code:    http.StatusForbidden,
	}
	return resp
}
