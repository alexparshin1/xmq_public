package controllers

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	_ "github.com/lib/pq"
	appsv1 "k8s.io/api/apps/v1"
	batchv1 "k8s.io/api/batch/v1"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/meta"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/apimachinery/pkg/api/resource"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/controller/controllerutil"
	"sigs.k8s.io/controller-runtime/pkg/log"

	xmqv1 "xmq.sptk.net/operator/api/v1"
)

// The operator generates the broker configuration for a known xmq.wsdl schema. A version outside
// this list may require new fields; refusing it is safer than starting a broker that cannot read
// the generated file.
var supportedVersions = map[string]bool{"1.2.1": true}

// XMQClusterReconciler owns the Kubernetes-side part of an XMQ cluster.
// Redis and PostgreSQL are external and are never created here.
type XMQClusterReconciler struct {
	client.Client
	Scheme *runtime.Scheme
}

// +kubebuilder:rbac:groups=xmq.sptk.net,resources=xmqclusters,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups=xmq.sptk.net,resources=xmqclusters/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=apps,resources=statefulsets,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups="",resources=configmaps;services;secrets,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups=batch,resources=jobs,verbs=get;list;watch;create;update;patch;delete

func (r *XMQClusterReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	logger := log.FromContext(ctx)

	var cluster xmqv1.XMQCluster
	if err := r.Get(ctx, req.NamespacedName, &cluster); err != nil {
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	if !supportedVersions[cluster.Spec.Version] {
		setCondition(&cluster, "Ready", false,
			fmt.Sprintf("XMQ %q is not supported by this operator; supported versions: 1.2.1", cluster.Spec.Version))
		if err := r.Status().Update(ctx, &cluster); err != nil {
			return ctrl.Result{}, err
		}
		return ctrl.Result{}, nil
	}

	replicas := cluster.Spec.Replicas
	if replicas == 0 {
		replicas = 3
	}

	if err := r.ensureConfigMaps(ctx, &cluster); err != nil {
		return ctrl.Result{}, err
	}
	if err := r.ensureServices(ctx, &cluster); err != nil {
		return ctrl.Result{}, err
	}
	if err := r.ensureStatefulSet(ctx, &cluster, replicas); err != nil {
		return ctrl.Result{}, err
	}

	var sts appsv1.StatefulSet
	if err := r.Get(ctx, types.NamespacedName{Name: cluster.Name, Namespace: cluster.Namespace}, &sts); err != nil {
		return ctrl.Result{}, err
	}
	cluster.Status.ReadyReplicas = sts.Status.ReadyReplicas
	cluster.Status.ObservedGeneration = cluster.Generation
	setCondition(&cluster, "StatefulSetReady", cluster.Status.ReadyReplicas >= replicas,
		fmt.Sprintf("%d of %d brokers ready", cluster.Status.ReadyReplicas, replicas))

	// Initial Setup needs a running broker, so wait for at least one pod before starting the Job.
	if cluster.Status.ReadyReplicas == 0 {
		if err := r.Status().Update(ctx, &cluster); err != nil {
			return ctrl.Result{}, err
		}
		return ctrl.Result{RequeueAfter: 10 * time.Second}, nil
	}

	if err := r.ensureSetupJob(ctx, &cluster); err != nil {
		return ctrl.Result{}, err
	}

	var job batchv1.Job
	if err := r.Get(ctx, types.NamespacedName{Name: cluster.Name + "-setup", Namespace: cluster.Namespace}, &job); err != nil {
		return ctrl.Result{}, err
	}
	setupDone := false
	for _, condition := range job.Status.Conditions {
		if condition.Type == batchv1.JobComplete && condition.Status == corev1.ConditionTrue {
			setupDone = true
		}
	}
	setCondition(&cluster, "SetupComplete", setupDone, "initial accounts password setup")

	if setupDone {
		if err := r.syncClusterCredential(ctx, &cluster); err != nil {
			logger.Error(err, "cannot sync the cluster credential")
			setCondition(&cluster, "ClusterCredentialReady", false, err.Error())
		} else {
			setCondition(&cluster, "ClusterCredentialReady", true, "outgoing cluster links use the configured password")
		}
	}

	ready := cluster.Status.ReadyReplicas >= replicas && setupDone &&
		meta.IsStatusConditionTrue(cluster.Status.Conditions, "ClusterCredentialReady")
	setCondition(&cluster, "Ready", ready, "cluster is serving clients with working cluster links")
	if err := r.Status().Update(ctx, &cluster); err != nil {
		return ctrl.Result{}, err
	}
	return ctrl.Result{RequeueAfter: 30 * time.Second}, nil
}

func setCondition(cluster *xmqv1.XMQCluster, kind string, ok bool, message string) {
	status := metav1.ConditionFalse
	reason := "NotReady"
	if ok {
		status = metav1.ConditionTrue
		reason = "Ready"
	}
	meta.SetStatusCondition(&cluster.Status.Conditions, metav1.Condition{
		Type: kind, Status: status, Reason: reason, Message: message,
		ObservedGeneration: cluster.Generation,
	})
}

func (r *XMQClusterReconciler) ensureConfigMaps(ctx context.Context, cluster *xmqv1.XMQCluster) error {
	configs := map[string]string{}
	replicas := cluster.Spec.Replicas
	if replicas == 0 {
		replicas = 3
	}
	for i := int32(0); i < replicas; i++ {
		config, err := brokerConfig(cluster, i)
		if err != nil {
			return err
		}
		configs[fmt.Sprintf("xmq-%d.conf", i)] = config
	}

	cm := &corev1.ConfigMap{ObjectMeta: metav1.ObjectMeta{Name: cluster.Name + "-configs", Namespace: cluster.Namespace}}
	_, err := controllerutil.CreateOrUpdate(ctx, r.Client, cm, func() error {
		cm.Data = configs
		return ctrl.SetControllerReference(cluster, cm, r.Scheme)
	})
	if err != nil {
		return err
	}

	ext := &corev1.ConfigMap{ObjectMeta: metav1.ObjectMeta{Name: cluster.Name + "-extensions", Namespace: cluster.Namespace}}
	_, err = controllerutil.CreateOrUpdate(ctx, r.Client, ext, func() error {
		ext.Data = map[string]string{"50-user-database.conf": userDatabaseExtension()}
		return ctrl.SetControllerReference(cluster, ext, r.Scheme)
	})
	return err
}

func brokerConfig(cluster *xmqv1.XMQCluster, i int32) (string, error) {
	persistence := true
	if cluster.Spec.EnablePersistence != nil {
		persistence = *cluster.Spec.EnablePersistence
	}
	config := map[string]any{
		"connections": map[string]any{
			"listener": []any{
				map[string]any{"id": 1, "name": "MQTT", "bind_ip": "0.0.0.0", "port": 1883, "threads": 4, "protocol": "MQTT", "enable": true},
				map[string]any{"id": 2, "name": "MQTT+SSL", "bind_ip": "0.0.0.0", "port": 8883, "threads": 4, "protocol": "MQTT+SSL", "enable": true},
			},
			"ssl_keys": map[string]any{
				"cafile": "/etc/xmq/certs/ca.crt", "keyfile": "/etc/xmq/certs/node.key",
				"certfile": "/etc/xmq/certs/node.crt", "verify_depth": "3",
			},
		},
		"queue_limits":   map[string]any{"max_size": 0, "max_inflight_messages": 0, "max_bytes": 0},
		"server_limits":  map[string]any{"send_threads": "auto", "receive_threads": "auto", "delivery_threads": "auto", "max_topic_alias": 128},
		"authentication": map[string]any{"allow_anonymous": true, "database_uri": cluster.Spec.DatabaseURI},
		"persistence": map[string]any{
			"redis_uri": cluster.Spec.RedisURI, "clean_start": false,
			"enabled": persistence, "max_redis_connections": 4, "max_queued_writes": 0,
		},
		"logging": map[string]any{
			"log_to": "xmq_server.log", "format": "json", "keep_logs": 7, "min_log_level": "INFO",
			"log_level": map[string]string{
				"connect": "INFO", "disconnect": "INFO", "subscribe": "INFO", "unsubscribe": "INFO",
				"publish": "INFO", "ack": "INFO", "server_connections": "INFO", "server_events": "INFO",
				"cluster_connections": "INFO", "cluster_events": "INFO",
			},
		},
		"web_service": map[string]any{
			"listener_port": 18883, "encrypted": true,
			"certfile": "/etc/xmq/certs/node.crt", "keyfile": "/etc/xmq/certs/node.key",
		},
		"prometheus": map[string]any{"enabled": true, "bind_ip": "0.0.0.0", "port": 9184},
		"bridges":    []any{},
		"cluster": map[string]any{
			"this_node": map[string]any{
				"node_name": fmt.Sprintf("%s-%d", cluster.Name, i),
				"host_port": fmt.Sprintf("%s-%d.%s.%s.svc.cluster.local:8883", cluster.Name, i, cluster.Name, cluster.Namespace),
				"encrypted": true,
			},
			"nodes": []any{}, "enabled": true, "lease_seconds": 10,
		},
	}
	data, err := json.MarshalIndent(config, "", "  ")
	if err != nil {
		return "", err
	}
	return string(data) + "\n", nil
}

func userDatabaseExtension() string {
	return `{
  "extensions": [
    {
      "name": "user-database",
      "library": "/usr/local/lib/xmq-extensions/libxmq_user_database.so",
      "enabled": true,
      "required": true,
      "settings": {"username": "", "password": "", "unknown_user": "deny"}
    }
  ]
}
`
}

func (r *XMQClusterReconciler) ensureServices(ctx context.Context, cluster *xmqv1.XMQCluster) error {
	selector := map[string]string{"app": cluster.Name}
	for _, svc := range []*corev1.Service{
		{
			ObjectMeta: metav1.ObjectMeta{Name: cluster.Name, Namespace: cluster.Namespace},
			Spec: corev1.ServiceSpec{
				ClusterIP: corev1.ClusterIPNone, Selector: selector,
				Ports: servicePorts(),
			},
		},
		{
			ObjectMeta: metav1.ObjectMeta{Name: cluster.Name + "-nodeport", Namespace: cluster.Namespace},
			Spec: corev1.ServiceSpec{
				Type: corev1.ServiceTypeNodePort, Selector: selector,
				Ports: servicePorts(),
			},
		},
	} {
		_, err := controllerutil.CreateOrUpdate(ctx, r.Client, svc, func() error {
			svc.Spec.Selector = selector
			svc.Spec.Ports = servicePorts()
			return ctrl.SetControllerReference(cluster, svc, r.Scheme)
		})
		if err != nil {
			return err
		}
	}
	return nil
}

func servicePorts() []corev1.ServicePort {
	return []corev1.ServicePort{
		{Name: "mqtt", Port: 1883},
		{Name: "mqtts", Port: 8883},
		{Name: "metrics", Port: 9184},
		{Name: "web", Port: 18883},
	}
}

func (r *XMQClusterReconciler) ensureStatefulSet(ctx context.Context, cluster *xmqv1.XMQCluster, replicas int32) error {
	image := cluster.Spec.Image
	if image == "" {
		image = "alexeyparshin/xmq:" + cluster.Spec.Version
	}
	sts := &appsv1.StatefulSet{ObjectMeta: metav1.ObjectMeta{Name: cluster.Name, Namespace: cluster.Namespace}}
	_, err := controllerutil.CreateOrUpdate(ctx, r.Client, sts, func() error {
		sts.Spec.ServiceName = cluster.Name
		sts.Spec.Replicas = &replicas
		// Ordered start avoids several nodes creating the SQL schema at once.
		sts.Spec.PodManagementPolicy = appsv1.OrderedReadyPodManagement
		sts.Spec.Selector = &metav1.LabelSelector{MatchLabels: map[string]string{"app": cluster.Name}}
		pod := corev1.PodTemplateSpec{
			ObjectMeta: metav1.ObjectMeta{Labels: map[string]string{"app": cluster.Name}},
		}
		init := corev1.Container{
			Name: "init", Image: "alpine:3.20",
			Command: []string{"/bin/sh", "-c"},
			Args: []string{`
set -eu
ordinal=${POD_NAME##*-}
cp /config/$POD_NAME.conf /etc/xmq/xmq_server.conf
if [ ! -f /etc/xmq/xmq_node.id ]; then cat /proc/sys/kernel/random/uuid > /etc/xmq/xmq_node.id; fi
mkdir -p /etc/xmq/certs
cp /ca/ca.crt /etc/xmq/certs/ca.crt
cp /tls/$ordinal/node.crt /etc/xmq/certs/node.crt
cp /tls/$ordinal/node.key /etc/xmq/certs/node.key
`},
			Env: []corev1.EnvVar{{Name: "POD_NAME", ValueFrom: &corev1.EnvVarSource{
				FieldRef: &corev1.ObjectFieldSelector{FieldPath: "metadata.name"}}}},
		}
		init.VolumeMounts = append(init.VolumeMounts, corev1.VolumeMount{Name: "config", MountPath: "/etc/xmq"})
		init.VolumeMounts = append(init.VolumeMounts, corev1.VolumeMount{Name: "configs", MountPath: "/config"})
		init.VolumeMounts = append(init.VolumeMounts, corev1.VolumeMount{Name: "ca", MountPath: "/ca"})
		for i := int32(0); i < replicas; i++ {
			init.VolumeMounts = append(init.VolumeMounts, corev1.VolumeMount{
				Name: fmt.Sprintf("tls-%d", i), MountPath: fmt.Sprintf("/tls/%d", i),
			})
		}

		main := corev1.Container{
			Name: "xmq", Image: image, ImagePullPolicy: corev1.PullIfNotPresent,
			Ports: []corev1.ContainerPort{
				{Name: "mqtt", ContainerPort: 1883}, {Name: "mqtts", ContainerPort: 8883},
				{Name: "web", ContainerPort: 18883}, {Name: "metrics", ContainerPort: 9184},
			},
			Env: []corev1.EnvVar{
				{Name: "XMQ_ALLOW_ANONYMOUS", Value: "true"},
				{Name: "XMQ_PERSISTENCE", Value: "true"},
				{Name: "XMQ_REDIS_URI", Value: cluster.Spec.RedisURI},
				{Name: "XMQ_PROMETHEUS", Value: "true"},
				{Name: "XMQ_LOG_FORMAT", Value: "json"},
			},
			LivenessProbe: &corev1.Probe{ProbeHandler: corev1.ProbeHandler{
				HTTPGet: &corev1.HTTPGetAction{Path: "/healthz", Port: intstrFromInt(9184)}},
				PeriodSeconds: 10},
			ReadinessProbe: &corev1.Probe{ProbeHandler: corev1.ProbeHandler{
				HTTPGet: &corev1.HTTPGetAction{Path: "/readyz", Port: intstrFromInt(9184)}},
				PeriodSeconds: 5},
			VolumeMounts: []corev1.VolumeMount{
				{Name: "config", MountPath: "/etc/xmq"},
				{Name: "extensions", MountPath: "/etc/xmq/xmq_extensions.d"},
			},
		}
		pod.Spec = corev1.PodSpec{
			InitContainers: []corev1.Container{init},
			Containers:     []corev1.Container{main},
			Volumes: []corev1.Volume{
				{Name: "configs", VolumeSource: corev1.VolumeSource{ConfigMap: &corev1.ConfigMapVolumeSource{
					LocalObjectReference: corev1.LocalObjectReference{Name: cluster.Name + "-configs"}}}},
				{Name: "extensions", VolumeSource: corev1.VolumeSource{ConfigMap: &corev1.ConfigMapVolumeSource{
					LocalObjectReference: corev1.LocalObjectReference{Name: cluster.Name + "-extensions"}}}},
				{Name: "ca", VolumeSource: corev1.VolumeSource{Secret: &corev1.SecretVolumeSource{
					SecretName: cluster.Spec.CASecret}}},
			},
		}
		for i := int32(0); i < replicas; i++ {
			pod.Spec.Volumes = append(pod.Spec.Volumes, corev1.Volume{
				Name: fmt.Sprintf("tls-%d", i),
				VolumeSource: corev1.VolumeSource{Secret: &corev1.SecretVolumeSource{
					SecretName: fmt.Sprintf("%s-%d", cluster.Spec.TLSSecretPrefix, i)}},
			})
		}
		sts.Spec.Template = pod
		sts.Spec.VolumeClaimTemplates = []corev1.PersistentVolumeClaim{{
			ObjectMeta: metav1.ObjectMeta{Name: "config"},
			Spec: corev1.PersistentVolumeClaimSpec{
				AccessModes: []corev1.PersistentVolumeAccessMode{corev1.ReadWriteOnce},
				Resources: corev1.VolumeResourceRequirements{
					Requests: corev1.ResourceList{corev1.ResourceStorage: resourceMustParse("1Gi")},
				},
			},
		}}
		return ctrl.SetControllerReference(cluster, sts, r.Scheme)
	})
	return err
}

func (r *XMQClusterReconciler) ensureSetupJob(ctx context.Context, cluster *xmqv1.XMQCluster) error {
	// A Job's pod template is immutable after creation. Create it once; on later reconciles only
	// its status is read by the caller.
	existing := &batchv1.Job{}
	if err := r.Get(ctx, types.NamespacedName{Name: cluster.Name + "-setup", Namespace: cluster.Namespace}, existing); err == nil {
		return nil
	} else if !apierrors.IsNotFound(err) {
		return err
	}

	image := cluster.Spec.Image
	if image == "" {
		image = "alexeyparshin/xmq:" + cluster.Spec.Version
	}
	labels := map[string]string{"app": cluster.Name, "job": "setup"}
	job := &batchv1.Job{ObjectMeta: metav1.ObjectMeta{Name: cluster.Name + "-setup", Namespace: cluster.Namespace}}
	_, err := controllerutil.CreateOrUpdate(ctx, r.Client, job, func() error {
		job.Spec.BackoffLimit = ptr(int32(3))
		job.Spec.Selector = &metav1.LabelSelector{MatchLabels: labels}
		container := corev1.Container{
			Name: "setup", Image: image, ImagePullPolicy: corev1.PullIfNotPresent,
			Command: []string{"/bin/sh", "-c"},
			Args: []string{`
set -eu
printf '%s\n' "$XMQ_ADMIN_PASSWORD"  | xmq_server -c /etc/xmq/xmq_server.conf --set-password admin
printf '%s\n' "$XMQ_CLUSTER_PASSWORD" | xmq_server -c /etc/xmq/xmq_server.conf --set-password cluster
`},
			Env: []corev1.EnvVar{
				{Name: "XMQ_ADMIN_PASSWORD", ValueFrom: secretKey(cluster.Spec.CredentialsSecret, "admin")},
				{Name: "XMQ_CLUSTER_PASSWORD", ValueFrom: secretKey(cluster.Spec.CredentialsSecret, "cluster")},
			},
			VolumeMounts: []corev1.VolumeMount{
				{Name: "config", MountPath: "/etc/xmq"},
				{Name: "configs", MountPath: "/config"},
			},
		}
		job.Spec.Template = corev1.PodTemplateSpec{
			ObjectMeta: metav1.ObjectMeta{Labels: labels},
			Spec: corev1.PodSpec{
				RestartPolicy: corev1.RestartPolicyNever,
				InitContainers: []corev1.Container{{
					Name: "config", Image: "alpine:3.20", Command: []string{"/bin/sh", "-c"},
					Args:         []string{"cp /config/xmq-0.conf /etc/xmq/xmq_server.conf"},
					VolumeMounts: container.VolumeMounts,
				}},
				Containers: []corev1.Container{container},
				Volumes: []corev1.Volume{
					{Name: "config", VolumeSource: corev1.VolumeSource{EmptyDir: &corev1.EmptyDirVolumeSource{}}},
					{Name: "configs", VolumeSource: corev1.VolumeSource{ConfigMap: &corev1.ConfigMapVolumeSource{
						LocalObjectReference: corev1.LocalObjectReference{Name: cluster.Name + "-configs"}}}},
				},
			},
		}
		return ctrl.SetControllerReference(cluster, job, r.Scheme)
	})
	return err
}

// syncClusterCredential stores the plain cluster secret used by outgoing links. The broker's
// CLI updates the account hash; this SQL row must contain the same password in plain text.
func (r *XMQClusterReconciler) syncClusterCredential(ctx context.Context, cluster *xmqv1.XMQCluster) error {
	var secret corev1.Secret
	if err := r.Get(ctx, types.NamespacedName{Name: cluster.Spec.CredentialsSecret, Namespace: cluster.Namespace}, &secret); err != nil {
		return err
	}
	password := string(secret.Data["cluster"])
	if password == "" {
		return fmt.Errorf("credentials secret %s has no cluster key", cluster.Spec.CredentialsSecret)
	}
	// lib/pq requires SSL by default; an external database may be plain HTTP-local.
	dsn := cluster.Spec.DatabaseURI
	if !strings.Contains(dsn, "sslmode=") {
		separator := "?"
		if strings.Contains(dsn, "?") {
			separator = "&"
		}
		dsn += separator + "sslmode=disable"
	}
	db, err := sql.Open("postgres", dsn)
	if err != nil {
		return err
	}
	defer db.Close()
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	_, err = db.ExecContext(ctx,
		`INSERT INTO xmq_cluster_credential (id, secret) VALUES (1, $1)
		 ON CONFLICT (id) DO UPDATE SET secret = EXCLUDED.secret`, password)
	return err
}

func secretKey(name, key string) *corev1.EnvVarSource {
	return &corev1.EnvVarSource{SecretKeyRef: &corev1.SecretKeySelector{
		LocalObjectReference: corev1.LocalObjectReference{Name: name}, Key: key}}
}

func ptr[T any](v T) *T { return &v }

func intstrFromInt(v int32) intstr.IntOrString { return intstr.FromInt32(v) }

func resourceMustParse(value string) resource.Quantity { return resource.MustParse(value) }

func (r *XMQClusterReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&xmqv1.XMQCluster{}).
		Owns(&appsv1.StatefulSet{}).
		Owns(&batchv1.Job{}).
		Complete(r)
}
