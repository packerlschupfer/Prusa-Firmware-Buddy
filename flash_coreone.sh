#!/bin/bash
# Flash custom Core One firmware
# Usage: ./flash_coreone.sh [swd|usb] [--skip-build]
#
# SWD mode: build → extract → st-flash → done (no USB needed!)
# USB mode: build → copy BBF to USB → eject → prompt to flash

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USB_MOUNT="/media/mrnice/PRUSA3D"
BBF_NAME="COREONE_firmware.bbf"
PRODUCT="$SCRIPT_DIR/build/products/coreone_release_emptyboot.bbf"
API_KEY="bhxtvZaC89XapUf"
PRINTER_IP="192.168.18.13"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

MODE="${1:-swd}"
SKIP_BUILD=false
for arg in "$@"; do
    [[ "$arg" == "--skip-build" ]] && SKIP_BUILD=true
    [[ "$arg" == "swd" ]] && MODE="swd"
    [[ "$arg" == "usb" ]] && MODE="usb"
done

echo -e "${GREEN}=== Core One Custom Firmware Flash Tool (${MODE}) ===${NC}"

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

if [[ "$MODE" == "swd" ]]; then
    # SWD flash: extract image and write directly
    echo -e "${YELLOW}Extracting flash image from BBF...${NC}"
    python3 -c "
import struct
with open('$PRODUCT', 'rb') as f:
    bbf = f.read()
sig = bbf[:64]
hash_hdr_fw = bbf[64:576 + struct.unpack_from('<I', bbf, 96)[0]]
with open('/tmp/swd_flash.bin', 'wb') as f:
    f.write(hash_hdr_fw + sig)
print(f'Flash image: {len(hash_hdr_fw) + 64} bytes')
"

    echo -e "${YELLOW}Flashing via SWD...${NC}"
    st-flash write /tmp/swd_flash.bin 0x08020000
    echo -e "${GREEN}Flash complete. Waiting for printer...${NC}"

else
    # USB flash
    echo -e "${YELLOW}Insert USB stick into PC...${NC}"
    while [[ ! -d "$USB_MOUNT" ]]; do sleep 1; done
    echo -e "${GREEN}USB detected.${NC}"
    rm -f "$USB_MOUNT"/*.bbf
    cp "$PRODUCT" "$USB_MOUNT/$BBF_NAME"
    sync
    echo -e "${GREEN}Firmware copied.${NC}"
    udisksctl unmount -b /dev/sda1 2>/dev/null || umount "$USB_MOUNT" 2>/dev/null || true
    echo -e "${GREEN}USB ejected.${NC}"
    echo ""
    echo "1. Insert USB stick into printer"
    echo "2. Reboot printer"
    echo "3. TAP screen when Prusa logo appears"
    echo "4. Confirm unofficial firmware"
    echo ""
fi

echo -e "${YELLOW}Waiting for printer to come online...${NC}"
while true; do
    if curl -s --connect-timeout 3 -H "X-Api-Key: $API_KEY" "http://$PRINTER_IP/api/v1/status" 2>/dev/null | \
       python3 -c "import sys,json; d=json.load(sys.stdin); print(f'Online: {d[\"printer\"][\"state\"]}  Chamber: {d.get(\"chamber\",{}).get(\"temp\",\"?\")}C')" 2>/dev/null; then
        break
    fi
    sleep 5
done
echo -e "${GREEN}=== Done! Custom firmware running. ===${NC}"
