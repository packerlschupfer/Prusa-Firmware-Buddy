#!/bin/bash
# Flash custom Core One firmware — fully unattended over network
# Usage: ./flash_coreone.sh [--skip-build]
#
# Workflow: build → upload BBF via API → M997 invalidate → auto-flash → done
# No USB swap, no screen tap, no SWD cable needed.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PRODUCT="$SCRIPT_DIR/build/products/coreone_release_emptyboot.bbf"
API_KEY="bhxtvZaC89XapUf"
PRINTER_IP="192.168.18.13"
BBF_NAME="COREONE_firmware.bbf"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

SKIP_BUILD=false
for arg in "$@"; do
    [[ "$arg" == "--skip-build" ]] && SKIP_BUILD=true
done

API="http://$PRINTER_IP"
HDR="-H X-Api-Key:$API_KEY"

echo -e "${GREEN}=== Core One Wireless Flash Tool ===${NC}"

# Step 1: Build
if ! $SKIP_BUILD; then
    echo -e "${YELLOW}Building firmware...${NC}"
    cd "$SCRIPT_DIR"
    python3 utils/build.py --preset coreone --bootloader empty --no-store-output --skip-bootstrap
    echo -e "${GREEN}Build complete.${NC}"
fi

if [[ ! -f "$PRODUCT" ]]; then
    echo -e "${RED}ERROR: BBF not found at $PRODUCT${NC}"
    exit 1
fi

# Step 2: Upload BBF
echo -e "${YELLOW}Uploading firmware ($(du -h "$PRODUCT" | cut -f1))...${NC}"
curl -s -X DELETE -H "X-Api-Key:$API_KEY" "$API/api/v1/files/usb/$BBF_NAME" 2>/dev/null || true
HTTP=$(curl -s -X PUT -H "X-Api-Key:$API_KEY" \
    -H "Content-Type: application/octet-stream" \
    -w "%{http_code}" -o /dev/null \
    --data-binary "@$PRODUCT" \
    "$API/api/v1/files/usb/$BBF_NAME")

if [[ "$HTTP" != "201" ]]; then
    echo -e "${RED}Upload failed (HTTP $HTTP)${NC}"
    exit 1
fi
echo -e "${GREEN}Upload complete.${NC}"

# Step 3: Invalidate firmware and reboot (M997)
echo -e "${YELLOW}Triggering firmware update (M997)...${NC}"
curl -s -X POST -H "X-Api-Key:$API_KEY" -H "Content-Type: application/json" \
    -d '{"gcode":"M997"}' "$API/api/v1/control" 2>/dev/null || true

# Step 4: Wait for printer to come back
echo -e "${YELLOW}Flashing... waiting for printer to reboot...${NC}"
sleep 30
for i in $(seq 1 24); do
    if STATUS=$(curl -s --connect-timeout 3 -H "X-Api-Key:$API_KEY" "$API/api/v1/status" 2>/dev/null); then
        STATE=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin)['printer']['state'])" 2>/dev/null)
        if [[ -n "$STATE" ]]; then
            echo -e "${GREEN}=== Printer online: $STATE ===${NC}"
            echo "$STATUS" | python3 -c "
import sys,json
d=json.load(sys.stdin)
p=d['printer']
c=d.get('chamber',{})
print(f'  Nozzle: {p[\"temp_nozzle\"]}C  Bed: {p[\"temp_bed\"]}C  Chamber: {c.get(\"temp\",\"?\")}C')
print(f'  Speed: {p[\"speed\"]}%  Flow: {p[\"flow\"]}%')
" 2>/dev/null
            exit 0
        fi
    fi
    sleep 5
done

echo -e "${RED}Printer did not come back online within 2 minutes${NC}"
exit 1
