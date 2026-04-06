#!/bin/bash
# Flash custom Core One firmware
# Usage: ./flash_coreone.sh [wifi|swd] [--skip-build]
#
# wifi (default): build → upload BBF → M997 invalidate → auto-flash (unattended)
# swd:            build → extract → st-flash → reboot (unattended, needs SWD cable)

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

MODE="wifi"
SKIP_BUILD=false
for arg in "$@"; do
    [[ "$arg" == "--skip-build" ]] && SKIP_BUILD=true
    [[ "$arg" == "wifi" ]] && MODE="wifi"
    [[ "$arg" == "swd" ]] && MODE="swd"
done

API="http://$PRINTER_IP"

echo -e "${GREEN}=== Core One Flash Tool (${MODE}) ===${NC}"

# Step 1: Build
if ! $SKIP_BUILD; then
    echo -e "${YELLOW}Building firmware...${NC}"
    cd "$SCRIPT_DIR"
    python3 utils/build.py --preset coreone --bootloader empty --no-store-output --skip-bootstrap
    echo -e "${GREEN}Build complete.${NC}"
fi

[[ ! -f "$PRODUCT" ]] && echo -e "${RED}BBF not found${NC}" && exit 1

if [[ "$MODE" == "swd" ]]; then
    # SWD: extract flash image and write directly
    echo -e "${YELLOW}Extracting flash image...${NC}"
    python3 -c "
import struct
with open('$PRODUCT', 'rb') as f:
    bbf = f.read()
sig = bbf[:64]
fw_size = struct.unpack_from('<I', bbf, 96)[0]
flash = bbf[64:576+fw_size] + sig
with open('/tmp/swd_flash.bin', 'wb') as f:
    f.write(flash)
print(f'Flash image: {len(flash)} bytes')
"
    echo -e "${YELLOW}Flashing via SWD...${NC}"
    st-flash write /tmp/swd_flash.bin 0x08020000

    echo -e "${YELLOW}Resetting MCU...${NC}"
    sleep 1
    # Clean all bootloader state and NVIC system reset:
    # 1. Clear fw_update_flag and set bootloader state in shared RAM
    # 2. Enable backup SRAM access (RCC_AHB1ENR bit 18, PWR_CR bit 8)
    # 3. Clear backup SRAM to prevent power panic false trigger
    # 4. NVIC system reset
    openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "init; halt" \
        -c "mwb 0x20000000 0x00; mwb 0x20000001 0x01; mwb 0x20000002 0x00; mwb 0x20000003 0x01" \
        -c "mmw 0x40023830 0x00040000 0; mmw 0x40007000 0x00000100 0" \
        -c "mww 0x40024000 0x00000000; mww 0x40024004 0x00000000; mww 0x40024008 0x00000000; mww 0x4002400C 0x00000000" \
        -c "mww 0xE000ED0C 0x05FA0004" \
        -c "shutdown" 2>/dev/null

else
    # WiFi: upload BBF, invalidate firmware, auto-flash
    echo -e "${YELLOW}Uploading firmware...${NC}"
    curl -s -X DELETE -H "X-Api-Key:$API_KEY" "$API/api/v1/files/usb/$BBF_NAME" 2>/dev/null || true
    HTTP=$(curl -s -X PUT -H "X-Api-Key:$API_KEY" \
        -H "Content-Type: application/octet-stream" \
        -w "%{http_code}" -o /dev/null \
        --data-binary "@$PRODUCT" \
        "$API/api/v1/files/usb/$BBF_NAME")
    [[ "$HTTP" != "201" ]] && echo -e "${RED}Upload failed (HTTP $HTTP)${NC}" && exit 1
    echo -e "${GREEN}Upload complete.${NC}"

    echo -e "${YELLOW}Triggering firmware update (M997)...${NC}"
    curl -s -X POST -H "X-Api-Key:$API_KEY" -H "Content-Type: application/json" \
        -d '{"gcode":"M997"}' "$API/api/v1/control" 2>/dev/null || true
fi

# Wait for printer
echo -e "${YELLOW}Waiting for printer...${NC}"
sleep 30
for i in $(seq 1 24); do
    if STATUS=$(curl -s --connect-timeout 3 -H "X-Api-Key:$API_KEY" "$API/api/v1/status" 2>/dev/null); then
        STATE=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin)['printer']['state'])" 2>/dev/null)
        if [[ -n "$STATE" ]]; then
            echo -e "${GREEN}=== Online: $STATE ===${NC}"
            echo "$STATUS" | python3 -c "
import sys,json; d=json.load(sys.stdin); p=d['printer']; c=d.get('chamber',{})
print(f'  Nozzle: {p[\"temp_nozzle\"]}C  Bed: {p[\"temp_bed\"]}C  Chamber: {c.get(\"temp\",\"?\")}C  Speed: {p[\"speed\"]}%')
" 2>/dev/null
            exit 0
        fi
    fi
    sleep 5
done
echo -e "${RED}Printer did not come back online${NC}"
exit 1
