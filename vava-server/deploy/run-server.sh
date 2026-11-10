#!/bin/sh
# Shared native/container launcher. Secrets are file paths, never CLI values.
set -eu
umask 077
server_bin="${VAVA_SERVER_BIN:-/usr/local/bin/vava-server}"
server_root="${VAVA_DATA_DIR:-/var/lib/vava-server}"
keys_file="${VAVA_DEVICE_KEYS:-/etc/vava-server/devices.keys}"
bootstrap="${VAVA_ADMIN_PASSWORD_FILE:-/etc/vava-server/initial-admin-password}"
web_bind="${VAVA_WEB_BIND:-127.0.0.1}"
cert="${VAVA_WEB_CERT:-}"
key="${VAVA_WEB_KEY:-}"
ffmpeg="${VAVA_FFMPEG:-/usr/bin/ffmpeg}"
[ -r "$keys_file" ] || { echo 'Missing device key registry; provision the server first.' >&2; exit 1; }
[ -z "$cert$key" ] || { [ -r "$cert" ] && [ -r "$key" ]; } || { echo 'Both TLS certificate and key must be readable.' >&2; exit 1; }
case "$web_bind" in 127.0.0.1) ;; *) [ -n "$cert" ] || { echo 'A non-loopback Web listener requires a TLS certificate and key.' >&2; exit 1; } ;; esac
mkdir -p "$server_root/state" "$server_root/recordings"
set -- --keys "$keys_file" --state-dir "$server_root/state" \
  --bind "${VAVA_UDP_BIND:-0.0.0.0}" --port "${VAVA_UDP_PORT:-3731}" \
  --file-dir "$server_root/recordings" --file-keyring "${VAVA_FILE_KEYRING:-$server_root/record-keys}" \
  --file-quota-mb "${VAVA_FILE_QUOTA_MB:-10240}" \
  --web-bind "$web_bind" --web-port "${VAVA_WEB_PORT:-8080}" \
  --web-dir "${VAVA_WEB_DIR:-/usr/local/share/vava-server/web}" \
  --web-admin-password-file "$bootstrap"
if [ -x "$ffmpeg" ]; then
  set -- "$@" --media-ffmpeg "$ffmpeg"
else
  echo 'FFmpeg is not executable; Web media conversion is unavailable.' >&2
fi
[ -z "$cert" ] || set -- "$@" --web-cert "$cert" --web-key "$key"
[ -z "${VAVA_WEB_ORIGIN:-}" ] || set -- "$@" --web-origin "$VAVA_WEB_ORIGIN"
exec "$server_bin" "$@"
