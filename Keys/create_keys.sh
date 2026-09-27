PASSWORD="12345"

echo Create CA key
openssl genrsa -des3 -out ca.key -passout "pass:$PASSWORD" 2048
echo Create a certificate for the CA
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt -subj '/C=AU/ST=VIC/L=Melbourne/O=Linotex/OU=XMQ/CN=xmq.linotex.net/emailAddress=xmq@linotex.net' -passin "pass:$PASSWORD"
echo Create a server key pair
openssl genrsa -out server.key 2048
echo Create a certificate request
openssl req -new -out server.csr -key server.key -subj '/C=AU/ST=VIC/L=Melbourne/O=Linotex/OU=XMQ/CN=xmq.linotex.net/emailAddress=xmq@linotex.net'
echo Create a certificate
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt -days 3650 -passin "pass:$PASSWORD"
