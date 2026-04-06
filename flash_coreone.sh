#!/bin/bash
# Flash custom Core One firmware via USB stick
# Usage: ./flash_coreone.sh [--build] [--skip-build]
#
# Workflow: build → copy BBF to USB → eject → prompt to flash

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USB_MOUNT="/media/mrnice/PRUSA3D"
BBF_NAME="COREONE_firmware.bbf"
PRODUCT="$SCRIPT_DIR/build/products/coreone_release_emptyboot.bbf"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}=== Core One Custom Firmware Flash Tool ===${NC}"

# Step 1: Build
if [[ "$1" != "--skip-build" ]]; then
    echo -e "${YELLOW}Building firmware...${NC}"
    cd "$SCRIPT_DIR"
    python3 utils/build.py --preset coreone --bootloader empty --no-store-output --skip-bootstrap
    echo -e "${GREEN}Build complete.${NC}"
else
    echo -e "${YELLOW}Skipping build.${NC}"
fi

if [[ ! -f "$PRODUCT" ]]; then
    echo -e "${RED}ERROR: BBF not found at $PRODUCT${NC}"
    exit 1
fi

# Step 2: Wait for USB stick
echo ""
echo -e "${YELLOW}Insert USB stick into PC...${NC}"
while [[ ! -d "$USB_MOUNT" ]]; do
    sleep 1
done
echo -e "${GREEN}USB stick detected at $USB_MOUNT${NC}"

# Step 3: Copy BBF
rm -f "$USB_MOUNT"/*.bbf
cp "$PRODUCT" "$USB_MOUNT/$BBF_NAME"
sync
echo -e "${GREEN}Firmware copied: $BBF_NAME ($(du -h "$USB_MOUNT/$BBF_NAME" | cut -f1))${NC}"

# Step 4: Eject
echo ""
echo -e "${YELLOW}Ejecting USB stick...${NC}"
udisksctl unmount -b /dev/sda1 2>/dev/null || umount "$USB_MOUNT" 2>/dev/null || true
echo -e "${GREEN}USB ejected.${NC}"

# Step 5: Prompt
echo ""
echo -e "${GREEN}=== Ready to flash ===${NC}"
echo "1. Insert USB stick into printer"
echo "2. Reboot printer (power cycle or reset button)"
echo "3. TAP the screen when the Prusa logo appears"
echo "4. Confirm 'unofficial firmware' on red screen"
echo ""
echo -e "${YELLOW}Waiting for printer to come online...${NC}"

API_KEY="bhxtvZaC89XapUf"
PRINTER_IP="192.168.18.13"

while true; do
    STATUS=$(curl -s --connect-timeout 3 -H "X-Api-Key: $API_KEY" "http://$PRINTER_IP/api/v1/status" 2>/dev/null)
    if echo "$STATUS" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['printer']['state'])" 2>/dev/null; then
        break
    fi
    sleep 5
done

echo -e "${GREEN}=== Printer online! Custom firmware running. ===${NC}"
echo "$STATUS" | python3 -m json.tool 2>/dev/null
