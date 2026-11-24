# libcrypto (OpenSSL's) for the app's tools: what git over ssh asks of it above
# all, ssh's key exchanges, host and user keys, ciphers and MACs (the ssh
# image's workload, ../ssh/ssh_train.sh, against the sshd record_app_images.sh
# starts), then TLS as python3 makes it (libssl over it: handshakes with RSA and
# EC keys, records with ChaCha20 and AES-GCM) and hashes.
sh /tmp/aojit/ssh/ssh_train.sh
cd /tmp/aojit/net || exit 1
sh gen_certs.sh
sh tls_run.sh 8450 rsa 20 40 4 2 8 ECDHE-RSA-CHACHA20-POLY1305
sh tls_run.sh 8451 ec 20 30 2 1 4 ECDHE-ECDSA-AES256-GCM-SHA384
python3 -c "
import hashlib, os
d = os.urandom(1 << 20)
for a in ('sha256', 'sha512', 'sha3_256', 'blake2b', 'sha1', 'md5'): hashlib.new(a, d).digest()
"
echo crypto-train-ok
