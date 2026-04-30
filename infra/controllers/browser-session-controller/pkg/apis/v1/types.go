// Package v1 — types.go
//
// CRD shape per T50's session-controller-design.md. Two resources:
//
//   BrowserSession      — one per active session
//   BrowserSessionPool  — the warm pool config
//
// Status fields are write-by-controller, read-by-clients.

package v1

import (
	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

// =========================================================================
// BrowserSession
// =========================================================================

// SessionPhase is the lifecycle phase reported on BrowserSession.status.
type SessionPhase string

const (
	// SessionPending — the controller has seen the CR but no Pod is
	// assigned yet. Either the warm pool is empty (cold-start path) or
	// the assignment is in flight.
	SessionPending SessionPhase = "Pending"

	// SessionWarming — the controller has materialised a fresh Pod for
	// this session and is waiting for it to pass readiness.
	SessionWarming SessionPhase = "Warming"

	// SessionReady — a Pod is assigned and bound; clients can connect.
	SessionReady SessionPhase = "Ready"

	// SessionDraining — idle / explicit-end fired; the controller is
	// terminating Chromium gracefully and tearing the Pod down.
	SessionDraining SessionPhase = "Draining"

	// SessionEnded — terminal. The CR can be deleted by the client at
	// any time after this.
	SessionEnded SessionPhase = "Ended"
)

// BrowserSessionSpec is the user-supplied request.
type BrowserSessionSpec struct {
	// TenantID is the tenant that owns this session. Mandatory once
	// signaling auth (T48) is wired into the admission webhook; for
	// auth-disabled dev deploys the admission webhook permits empty
	// and translates it to "_anonymous".
	// +kubebuilder:validation:MaxLength=64
	// +kubebuilder:validation:Pattern=`^[a-zA-Z0-9_-]*$`
	TenantID string `json:"tenantID,omitempty"`

	// PoolName is the BrowserSessionPool to draw a warm Pod from. If
	// empty the controller falls back to "default-pool" in the same
	// namespace.
	PoolName string `json:"poolName,omitempty"`

	// Region is a free-form hint for which region's pool to consult
	// (e.g. "us-east-1"). Pools advertise their region via labels;
	// the controller filters Pods accordingly. Empty matches any.
	Region string `json:"region,omitempty"`

	// IdleTimeoutSeconds is the cluster-side idle eviction timeout.
	// The controller marks the session Draining when no signaling
	// activity has been seen for this many seconds.
	// +kubebuilder:default=600
	// +kubebuilder:validation:Minimum=30
	IdleTimeoutSeconds int32 `json:"idleTimeoutSeconds,omitempty"`
}

// SessionConnection is the client-facing connection info the
// controller returns once a Pod is assigned. The API gateway reads
// these and hands them to the user.
type SessionConnection struct {
	// SignalingURL is the WebSocket URL the client dials with this
	// session_id appended.
	SignalingURL string `json:"signalingURL,omitempty"`

	// PodName is the assigned Pod's name. Useful for kubectl debug
	// flows; clients should treat the Pod as opaque.
	PodName string `json:"podName,omitempty"`

	// PodIP is the assigned Pod's cluster IP. Surfaced for API
	// gateways that prefer to construct their own ingress URLs.
	PodIP string `json:"podIP,omitempty"`
}

// BrowserSessionStatus is the controller's view of the session.
type BrowserSessionStatus struct {
	// Phase is the high-level lifecycle marker. See SessionPhase.
	Phase SessionPhase `json:"phase,omitempty"`

	// PodName + connection info filled when Phase == Ready.
	Connection *SessionConnection `json:"connection,omitempty"`

	// LastActivityAt is the most recent signaling-server-reported
	// timestamp for this session. The controller polls signaling
	// metrics + listens for webhook pings (Phase 3 follow-up). Used
	// for cluster-side idle eviction.
	LastActivityAt *metav1.Time `json:"lastActivityAt,omitempty"`

	// StartedAt is the time the session entered Ready phase.
	StartedAt *metav1.Time `json:"startedAt,omitempty"`

	// EndedAt is the time the session entered Ended phase.
	EndedAt *metav1.Time `json:"endedAt,omitempty"`

	// EndReason is a short string explaining why the session ended.
	// One of: "ClientDelete", "IdleTimeout", "PodEviction",
	// "ControllerError".
	EndReason string `json:"endReason,omitempty"`

	// Conditions holds detailed status conditions per the standard
	// K8s convention.
	Conditions []metav1.Condition `json:"conditions,omitempty"`
}

// +kubebuilder:object:root=true
// +kubebuilder:subresource:status
// +kubebuilder:printcolumn:name="Phase",type=string,JSONPath=".status.phase"
// +kubebuilder:printcolumn:name="Pod",type=string,JSONPath=".status.connection.podName"
// +kubebuilder:printcolumn:name="Tenant",type=string,JSONPath=".spec.tenantID"
// +kubebuilder:printcolumn:name="Age",type=date,JSONPath=".metadata.creationTimestamp"

// BrowserSession is one cloud-browser session.
type BrowserSession struct {
	metav1.TypeMeta   `json:",inline"`
	metav1.ObjectMeta `json:"metadata,omitempty"`

	Spec   BrowserSessionSpec   `json:"spec,omitempty"`
	Status BrowserSessionStatus `json:"status,omitempty"`
}

// +kubebuilder:object:root=true

// BrowserSessionList wraps a list of BrowserSession.
type BrowserSessionList struct {
	metav1.TypeMeta `json:",inline"`
	metav1.ListMeta `json:"metadata,omitempty"`
	Items           []BrowserSession `json:"items"`
}

// =========================================================================
// BrowserSessionPool
// =========================================================================

// PoolRecyclePolicy controls what happens to a Pod when its session
// ends. ScrubAndReturn is faster but only safe for single-tenant
// pools; RecreatePod is the default per T50's design doc.
type PoolRecyclePolicy string

const (
	// PoolRecreatePod tears the Pod down on session end. Default.
	// One tenant per Pod, ever.
	PoolRecreatePod PoolRecyclePolicy = "RecreatePod"

	// PoolScrubAndReturn restarts Chromium in-place and returns the
	// Pod to the warm pool. Only safe inside a single-tenant
	// boundary.
	PoolScrubAndReturn PoolRecyclePolicy = "ScrubAndReturn"
)

// BrowserSessionPoolSpec configures the pool.
type BrowserSessionPoolSpec struct {
	// WarmReplicas is the number of pre-created Pods we try to keep
	// in cb.session/state=warm.
	// +kubebuilder:validation:Minimum=0
	// +kubebuilder:validation:Maximum=10000
	WarmReplicas int32 `json:"warmReplicas"`

	// MaxSessions caps the total number of active+warm Pods this
	// pool may schedule. Beyond this the controller refuses
	// assignments.
	// +kubebuilder:validation:Minimum=0
	MaxSessions int32 `json:"maxSessions,omitempty"`

	// MaxAgeSeconds is the lifetime above which a warm Pod is drained
	// and replaced. Lets us roll image updates without disrupting
	// active sessions; warm Pods cycle on their own.
	// +kubebuilder:default=3600
	MaxAgeSeconds int32 `json:"maxAgeSeconds,omitempty"`

	// RecyclePolicy controls Pod handling on session end. Default
	// RecreatePod.
	// +kubebuilder:default=RecreatePod
	// +kubebuilder:validation:Enum=RecreatePod;ScrubAndReturn
	RecyclePolicy PoolRecyclePolicy `json:"recyclePolicy,omitempty"`

	// Template is the Pod template materialised for each warm Pod.
	// Same shape as a Deployment's PodTemplate.
	Template corev1.PodTemplateSpec `json:"template"`
}

// BrowserSessionPoolStatus reports the pool's current capacity.
type BrowserSessionPoolStatus struct {
	// Warm is the count of Pods labelled cb.session/state=warm.
	Warm int32 `json:"warm"`

	// Active is the count labelled cb.session/state=assigned.
	Active int32 `json:"active"`

	// Draining is the count labelled cb.session/state=draining.
	Draining int32 `json:"draining"`

	// TotalEverProvisioned is a monotonic counter; useful for
	// dashboards and capacity-planning histograms.
	TotalEverProvisioned int64 `json:"totalEverProvisioned"`

	// LastReplenishedAt records the last replenishment-loop tick.
	LastReplenishedAt *metav1.Time `json:"lastReplenishedAt,omitempty"`

	// Conditions per K8s convention.
	Conditions []metav1.Condition `json:"conditions,omitempty"`
}

// +kubebuilder:object:root=true
// +kubebuilder:subresource:status
// +kubebuilder:printcolumn:name="Warm",type=integer,JSONPath=".status.warm"
// +kubebuilder:printcolumn:name="Active",type=integer,JSONPath=".status.active"
// +kubebuilder:printcolumn:name="Draining",type=integer,JSONPath=".status.draining"
// +kubebuilder:printcolumn:name="Target",type=integer,JSONPath=".spec.warmReplicas"

// BrowserSessionPool is the warm pool config.
type BrowserSessionPool struct {
	metav1.TypeMeta   `json:",inline"`
	metav1.ObjectMeta `json:"metadata,omitempty"`

	Spec   BrowserSessionPoolSpec   `json:"spec,omitempty"`
	Status BrowserSessionPoolStatus `json:"status,omitempty"`
}

// +kubebuilder:object:root=true

// BrowserSessionPoolList wraps a list of BrowserSessionPool.
type BrowserSessionPoolList struct {
	metav1.TypeMeta `json:",inline"`
	metav1.ListMeta `json:"metadata,omitempty"`
	Items           []BrowserSessionPool `json:"items"`
}

// =========================================================================
// Pod label / annotation conventions
// =========================================================================

// Label keys and well-known values used by the controller. The
// `cb.session/` namespace is reserved for controller-managed labels;
// `cb.io/` for annotations the wider ecosystem (snapshot helper, API
// gateway, etc.) reads or writes.
const (
	LabelSessionState       = "cb.session/state"
	LabelSessionStateWarm   = "warm"
	LabelSessionStateAssign = "assigned"
	LabelSessionStateDrain  = "draining"

	LabelSessionOwner = "cb.session/owner"      // session CR name when assigned
	LabelSessionPool  = "cb.session/pool"       // pool CR name
	LabelSessionTenant = "cb.session/tenant"    // tenant id, "_anonymous" if none

	AnnotationSnapshotID = "cb.io/snapshot-id"  // T68
	AnnotationSessionID  = "cb.io/session-id"   // session CR uid mirror

	// AnonymousTenant matches signaling/auth.go's anonymousTenant
	// constant. Two systems, same string — keep in sync if either
	// changes.
	AnonymousTenant = "_anonymous"
)

func init() {
	SchemeBuilder.Register(&BrowserSession{}, &BrowserSessionList{})
	SchemeBuilder.Register(&BrowserSessionPool{}, &BrowserSessionPoolList{})
}
