mosquitto_pub -i test_client -L mqtt://user:secret@localhost:8883/topic1 -q 0 -i test000 -V 5 \
  --insecure --cafile /etc/mosquitto/certs/server.crt --tls-version tlsv1.2 \
  -c -d -m test
