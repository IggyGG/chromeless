// Package v1 contains the API definitions for the
// cloud-browser-webrtc.example.com/v1alpha1 group, namely
// BrowserSession and BrowserSessionPool.
//
// We follow the kubebuilder pattern: SchemeGroupVersion + AddToScheme
// here, the actual struct definitions in types.go.
//
// +groupName=cloud-browser-webrtc.example.com
// +versionName=v1alpha1
// +kubebuilder:object:generate=true
package v1

import (
	"k8s.io/apimachinery/pkg/runtime/schema"
	"sigs.k8s.io/controller-runtime/pkg/scheme"
)

// GroupVersion is the API group + version for these types.
var GroupVersion = schema.GroupVersion{
	Group:   "cloud-browser-webrtc.example.com",
	Version: "v1alpha1",
}

// SchemeBuilder is the runtime.Scheme registrar shared by main and
// reconciler packages.
var SchemeBuilder = &scheme.Builder{GroupVersion: GroupVersion}

// AddToScheme adds the types in this package to the given scheme.
var AddToScheme = SchemeBuilder.AddToScheme
