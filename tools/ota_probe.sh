#!/usr/bin/env bash
# Probe the OTA host server running on a badge AP (192.168.4.1).
# Connect to WHY2025-open first, then run this script.
# Usage: ./ota_probe.sh [host] [slug]

HOST=${1:-http://192.168.4.1}
SLUG=${2:-}

die() { echo "error: $*" >&2; exit 1; }

# Strip trailing slash
HOST=${HOST%/}

echo "=== ping ==="
curl -sf "$HOST/api/v3/ping" || die "ping failed — are you connected to WHY2025-open?"
echo

echo "=== project-summaries ==="
SUMMARIES=$(curl -sf "$HOST/api/v3/project-summaries") || die "summaries failed"
echo "$SUMMARIES" | python3 -m json.tool 2>/dev/null || echo "$SUMMARIES"
echo

echo "=== firmware ==="
FW_REV=$(curl -sf "$HOST/api/v3/project-latest-revisions/why2025_firmware") \
    || die "firmware revision failed"
FW_VER=$(curl -sf "$HOST/api/v3/projects/why2025_firmware/rev${FW_REV}/files/version.txt") \
    || die "firmware version failed"
echo "  revision : $FW_REV"
echo "  version  : $FW_VER"
echo

if [ -n "$SLUG" ]; then
    echo "=== app: $SLUG ==="
    APP_REV=$(curl -sf "$HOST/api/v3/project-latest-revisions/$SLUG") \
        || die "app revision failed for $SLUG"
    echo "  revision : $APP_REV"
    APP_JSON=$(curl -sf "$HOST/api/v3/projects/$SLUG/rev${APP_REV}") \
        || die "app revision JSON failed for $SLUG"
    echo "$APP_JSON" | python3 -m json.tool 2>/dev/null || echo "$APP_JSON"
    echo
fi
