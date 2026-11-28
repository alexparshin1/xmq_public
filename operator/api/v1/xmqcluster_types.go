package v1

import (
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

// XMQClusterSpec describes the desired broker cluster. Redis and PostgreSQL are external:
// the operator only connects to them, it never creates or manages them.
type XMQClusterSpec struct {
	// Number of broker pods. Stable names and per-pod storage are provided by a StatefulSet.
	// +kubebuilder:validation:Minimum=1
	Replicas int32 `json:"replicas,omitempty"`

	// XMQ version, for example 1.2.1. Used in the image tag and in the record of the cluster.
	// +kubebuilder:validation:Required
	Version string `json:"version"`

	// Image overrides the default alexeyparshin/xmq:<version>.
	Image string `json:"image,omitempty"`

	// RedisURI is the shared runtime storage, for example redis://redis:6379/0.
	RedisURI string `json:"redisURI"`

	// DatabaseURI is the shared PostgreSQL configuration/accounts database.
	// It is passed to the broker verbatim; protect it with Kubernetes RBAC.
	DatabaseURI string `json:"databaseURI"`

	// CASecret names a Secret with a ca.crt key, signed by the common CA.
	CASecret string `json:"caSecret"`

	// TLSSecretPrefix is followed by -0, -1, ... Each Secret has node.crt and node.key.
	TLSSecretPrefix string `json:"tlsSecretPrefix"`

	// CredentialsSecret has keys "admin" and "cluster" with the two passwords.
	CredentialsSecret string `json:"credentialsSecret"`

	// EnablePersistence turns on persistence in every broker. Defaults to true.
	EnablePersistence *bool `json:"enablePersistence,omitempty"`
}

// XMQClusterStatus reports what the operator has achieved.
type XMQClusterStatus struct {
	// Conditions: Ready and SetupComplete.
	Conditions []metav1.Condition `json:"conditions,omitempty"`

	ReadyReplicas int32  `json:"readyReplicas,omitempty"`
	ObservedGeneration int64 `json:"observedGeneration,omitempty"`
}

// +kubebuilder:object:root=true
// +kubebuilder:subresource:status
// +kubebuilder:resource:shortName=xmq

type XMQCluster struct {
	metav1.TypeMeta   `json:",inline"`
	metav1.ObjectMeta `json:"metadata,omitempty"`

	Spec   XMQClusterSpec   `json:"spec,omitempty"`
	Status XMQClusterStatus `json:"status,omitempty"`
}

// +kubebuilder:object:root=true

type XMQClusterList struct {
	metav1.TypeMeta `json:",inline"`
	metav1.ListMeta `json:"metadata,omitempty"`
	Items           []XMQCluster `json:"items"`
}

func init() { SchemeBuilder.Register(&XMQCluster{}, &XMQClusterList{}) }
