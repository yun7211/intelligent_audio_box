# Local HTTPS signaling — TLS certificates (REQUIRED)

The device-hosted signaling server (`webrtc_http_server.c`) serves over **HTTPS**
because browsers only grant microphone/camera access (`getUserMedia`) on a secure
origin. It embeds a self-signed certificate + private key via `EMBED_TXTFILES`
(see `main/CMakeLists.txt`).

**These two files must exist here or the build (with the feature on + local
signaling) will fail at the EMBED step:**

- `servercert.pem`
- `prvtkey.pem`

They are intentionally NOT committed (a self-signed dev key is public and
insecure). Generate a pair with OpenSSL:

```sh
cd main/webrtc/signaling/http_local
mkdir -p certs
# On Windows Git Bash, prefix with MSYS_NO_PATHCONV=1 so /CN is not path-mangled.
MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 \
    -keyout certs/prvtkey.pem -out certs/servercert.pem \
    -days 3650 -nodes -subj "/CN=xiaozhi.local"
```

Notes:
- The self-signed cert triggers a one-time "not secure" warning in the phone
  browser; accept it to proceed. Only used for LAN testing.
- For anything beyond testing, provision a proper certificate.
