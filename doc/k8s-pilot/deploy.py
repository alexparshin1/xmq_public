import json, pathlib, subprocess, sys, os

DB_PASSWORD = os.environ.get("XMQ_DB_PASSWORD")
if not DB_PASSWORD:
    raise SystemExit("set XMQ_DB_PASSWORD before running deploy.py")

D = pathlib.Path.home() / "k8s-model"
D.mkdir(parents=True, exist_ok=True)

def base_config(i):
    return {
        "connections": {
            "listener": [
                {"id": 1, "name": "MQTT", "bind_ip": "0.0.0.0", "port": 1883, "threads": 4, "protocol": "MQTT", "enable": True},
                {"id": 2, "name": "MQTT+SSL", "bind_ip": "0.0.0.0", "port": 8883, "threads": 4, "protocol": "MQTT+SSL", "enable": True},
            ],
            "ssl_keys": {
                "cafile": "/etc/xmq/certs/ca.crt",
                "keyfile": "/etc/xmq/certs/node.key",
                "certfile": "/etc/xmq/certs/node.crt",
                "verify_depth": "3",
            },
        },
        "queue_limits": {"max_size": 0, "max_inflight_messages": 0, "max_bytes": 0},
        "server_limits": {"send_threads": "auto", "receive_threads": "auto", "delivery_threads": "auto", "max_topic_alias": 128},
        "authentication": {
            "allow_anonymous": True,
            "database_uri": f"postgresql://xmq:{DB_PASSWORD}@postgres:5432/xmq",
        },
        "persistence": {
            "redis_uri": "redis://redis:6379/0",
            "clean_start": False,
            "enabled": True,
            "max_redis_connections": 4,
            "max_queued_writes": 0,
        },
        "logging": {
            "log_to": "xmq_server.log",
            "format": "json",
            "keep_logs": 7,
            "min_log_level": "INFO",
            "log_level": {
                "connect": "INFO", "disconnect": "INFO", "subscribe": "INFO", "unsubscribe": "INFO",
                "publish": "INFO", "ack": "INFO", "server_connections": "INFO", "server_events": "INFO",
                "cluster_connections": "INFO", "cluster_events": "INFO",
            },
        },
        "web_service": {
            "listener_port": 18883,
            "encrypted": True,
            "certfile": "/etc/xmq/certs/node.crt",
            "keyfile": "/etc/xmq/certs/node.key",
        },
        "prometheus": {"enabled": True, "bind_ip": "0.0.0.0", "port": 9184},
        "bridges": [],
        "cluster": {
            "this_node": {
                "node_name": f"xmq-{i}",
                "host_port": f"xmq-{i}.xmq.xmq.svc.cluster.local:8883",
                "encrypted": True,
            },
            "nodes": [],
            "enabled": True,
            "lease_seconds": 10,
        },
    }

configs = {}
for i in range(3):
    configs[f"xmq-{i}.conf"] = json.dumps(base_config(i), indent=2) + "\n"

(D / "configs").mkdir(exist_ok=True)
for name, text in configs.items():
    (D / "configs" / name).write_text(text)

def apply(text):
    subprocess.run(["kubectl", "apply", "-f", "-"], input=text, text=True, check=True)

apply("""apiVersion: v1
kind: ConfigMap
metadata:
  name: xmq-configs
  namespace: xmq
data:
""" + "".join(f"  {k}: |\n" + "".join("    " + line + "\n" for line in v.splitlines()) for k, v in configs.items()))

apply("""apiVersion: v1
kind: ConfigMap
metadata: {name: xmq-ext, namespace: xmq}
data:
  50-user-database.conf: |
    {
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
""")

apply("""apiVersion: apps/v1
kind: Deployment
metadata: {name: redis, namespace: xmq}
spec:
  replicas: 1
  selector: {matchLabels: {app: redis}}
  template:
    metadata: {labels: {app: redis}}
    spec:
      containers:
      - name: redis
        image: redis:8-alpine
        command: ["redis-server", "--appendonly", "yes"]
        ports: [{containerPort: 6379}]
---
apiVersion: v1
kind: Service
metadata: {name: redis, namespace: xmq}
spec:
  selector: {app: redis}
  ports: [{port: 6379, targetPort: 6379}]
""")

apply(f"""apiVersion: apps/v1
kind: Deployment
metadata: {{name: postgres, namespace: xmq}}
spec:
  replicas: 1
  selector: {{matchLabels: {{app: postgres}}}}
  template:
    metadata: {{labels: {{app: postgres}}}}
    spec:
      containers:
      - name: postgres
        image: postgres:16-alpine
        env:
        - {{name: POSTGRES_USER, value: xmq}}
        - {{name: POSTGRES_PASSWORD, value: {DB_PASSWORD}}}
        - {{name: POSTGRES_DB, value: xmq}}
        ports: [{{containerPort: 5432}}]
---
apiVersion: v1
kind: Service
metadata: {{name: postgres, namespace: xmq}}
spec:
  selector: {{app: postgres}}
  ports: [{{port: 5432, targetPort: 5432}}]
""")

apply("""apiVersion: v1
kind: Service
metadata: {name: xmq, namespace: xmq}
spec:
  clusterIP: None
  selector: {app: xmq}
  ports:
  - {name: mqtt, port: 1883, targetPort: 1883}
  - {name: mqtts, port: 8883, targetPort: 8883}
  - {name: metrics, port: 9184, targetPort: 9184}
  - {name: web, port: 18883, targetPort: 18883}
---
apiVersion: v1
kind: Service
metadata: {name: xmq-nodeport, namespace: xmq}
spec:
  type: NodePort
  selector: {app: xmq}
  ports:
  - {name: mqtt, port: 1883, targetPort: 1883, nodePort: 30083}
  - {name: mqtts, port: 8883, targetPort: 8883, nodePort: 30883}
  - {name: metrics, port: 9184, targetPort: 9184, nodePort: 30184}
  - {name: web, port: 18883, targetPort: 18883, nodePort: 30185}
""")

sts = """apiVersion: apps/v1
kind: StatefulSet
metadata: {name: xmq, namespace: xmq}
spec:
  serviceName: xmq
  replicas: 3
  podManagementPolicy: Parallel
  selector: {matchLabels: {app: xmq}}
  template:
    metadata: {labels: {app: xmq}}
    spec:
      initContainers:
      - name: init
        image: alpine:3.20
        command: ["/bin/sh", "-c"]
        args:
        - |
          set -eu
          ordinal=${POD_NAME##*-}
          cp /config/$POD_NAME.conf /etc/xmq/xmq_server.conf
          if [ ! -f /etc/xmq/xmq_node.id ]; then cat /proc/sys/kernel/random/uuid > /etc/xmq/xmq_node.id; fi
          mkdir -p /etc/xmq/certs
          cp /ca/ca.crt /etc/xmq/certs/ca.crt
          cp /tls/$ordinal/node.crt /etc/xmq/certs/node.crt
          cp /tls/$ordinal/node.key /etc/xmq/certs/node.key
        env:
        - name: POD_NAME
          valueFrom: {fieldRef: {fieldPath: metadata.name}}
        volumeMounts:
        - {name: config, mountPath: /etc/xmq}
        - {name: configs, mountPath: /config}
        - {name: ca, mountPath: /ca}
        - {name: tls0, mountPath: /tls/0}
        - {name: tls1, mountPath: /tls/1}
        - {name: tls2, mountPath: /tls/2}
      containers:
      - name: xmq
        image: alexeyparshin/xmq:1.2.1
        imagePullPolicy: IfNotPresent
        ports:
        - {name: mqtt, containerPort: 1883}
        - {name: mqtts, containerPort: 8883}
        - {name: web, containerPort: 18883}
        - {name: metrics, containerPort: 9184}
        env:
        - {name: XMQ_ALLOW_ANONYMOUS, value: "true"}
        - {name: XMQ_PERSISTENCE, value: "true"}
        - {name: XMQ_REDIS_URI, value: "redis://redis:6379/0"}
        - {name: XMQ_PROMETHEUS, value: "true"}
        - {name: XMQ_LOG_FORMAT, value: "json"}
        volumeMounts:
        - {name: config, mountPath: /etc/xmq}
        - {name: ext, mountPath: /etc/xmq/xmq_extensions.d}
        livenessProbe:
          httpGet: {path: /healthz, port: 9184}
          periodSeconds: 10
        readinessProbe:
          httpGet: {path: /readyz, port: 9184}
          periodSeconds: 5
      volumes:
      - name: configs
        configMap: {name: xmq-configs}
      - name: ext
        configMap: {name: xmq-ext}
      - name: ca
        secret: {secretName: xmq-ca}
      - name: tls0
        secret: {secretName: xmq-tls-0}
      - name: tls1
        secret: {secretName: xmq-tls-1}
      - name: tls2
        secret: {secretName: xmq-tls-2}
  volumeClaimTemplates:
  - metadata: {name: config}
    spec:
      accessModes: ["ReadWriteOnce"]
      storageClassName: standard
      resources: {requests: {storage: 1Gi}}
"""
apply(sts)
print("manifests applied")
