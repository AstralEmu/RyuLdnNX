#!/usr/bin/env bash
# wait-for-server.sh — Wait for LdnServer to accept TCP connections
# Uses python3 (no nc dependency)
# Usage: ./wait-for-server.sh [host] [port] [timeout_seconds]
set -e

HOST="${1:-localhost}"
PORT="${2:-30456}"
TIMEOUT="${3:-60}"

echo "[wait] Waiting for LdnServer at ${HOST}:${PORT} (timeout: ${TIMEOUT}s)"

# A restarting container accepts the connection through the Docker port proxy
# and closes it again until the application listens, so a connect alone proves
# nothing: the connection must also stay open for one second (the server never
# speaks first).
check_tcp() {
    python3 -c "
import select, socket, sys
s = socket.socket()
s.settimeout(2)
try:
    s.connect((sys.argv[1], int(sys.argv[2])))
    sys.exit(1 if select.select([s], [], [], 1)[0] else 0)
except OSError:
    sys.exit(1)
finally:
    s.close()
" "$HOST" "$PORT" 2>/dev/null
}

elapsed=0
while ! check_tcp; do
    if [ "$elapsed" -ge "$TIMEOUT" ]; then
        echo "[wait] TIMEOUT — server not ready after ${TIMEOUT}s" >&2
        exit 1
    fi
    sleep 1
    elapsed=$((elapsed + 1))
    printf "[wait] Still waiting... (%ds)\r" "$elapsed"
done

echo "[wait] Server is ready at ${HOST}:${PORT} (took ~${elapsed}s)"