#!/bin/sh
set -u
matches=$(rg -n \
  'AmiSSLBase|AmiSSLExtBase|AmiSSLMasterBase|SocketBase|SSL_new|SSL_connect|SSL_read|SSL_write|SSL_free|SSL_CTX_|CloseSocket' \
  --glob '*.c' --glob '*.h' \
  --glob '!radio_stream.c' \
  --glob '!amissl*' \
  --glob '!tests/**' \
  --glob '!docs/**' \
  --glob '!Makefile*' \
  "$@")
status=$?
if [ "$status" -eq 1 ]; then
  exit 0
fi
if [ "$status" -ne 0 ]; then
  printf '%s\n' "$matches"
  exit "$status"
fi
printf '%s\n' "$matches"
exit 1
