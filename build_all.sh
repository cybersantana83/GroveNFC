#!/usr/bin/env sh
set -u

# Build all PlatformIO environments and merge firmware binaries for M5Burner deployment.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR="$SCRIPT_DIR"
INI_FILE="$PROJECT_DIR/platformio.ini"
OUTPUT_DIR="$PROJECT_DIR/output"
DATE_TAG=$(date +%Y%m%d)

if [ ! -f "$INI_FILE" ]; then
  echo "Error: platformio.ini not found in $PROJECT_DIR"
  exit 1
fi

if command -v pio >/dev/null 2>&1; then
  PIO_CMD="pio"
elif command -v platformio >/dev/null 2>&1; then
  PIO_CMD="platformio"
else
  echo "Error: PlatformIO CLI not found. Please install it first."
  echo "Install: pip install platformio"
  exit 1
fi

ENVS=$(sed -n 's/^\[env:\([^]]*\)\]$/\1/p' "$INI_FILE")

if [ -z "$ENVS" ]; then
  echo "Error: no [env:...] sections found in platformio.ini"
  exit 1
fi

mkdir -p "$OUTPUT_DIR"

FAILED_ENVS=""
TOTAL=0
SUCCESS=0

# ========== Build ==========
for ENV_NAME in $ENVS; do
  TOTAL=$((TOTAL + 1))
  echo ""
  echo "========================================"
  echo "Building environment: $ENV_NAME"
  echo "========================================"

  if "$PIO_CMD" run -d "$PROJECT_DIR" -e "$ENV_NAME"; then
    SUCCESS=$((SUCCESS + 1))
    echo "[OK] $ENV_NAME"
  else
    FAILED_ENVS="$FAILED_ENVS $ENV_NAME"
    echo "[FAIL] $ENV_NAME"
  fi
done

echo ""
echo "Build summary: $SUCCESS/$TOTAL succeeded."

if [ -n "$FAILED_ENVS" ]; then
  echo "Failed environments:$FAILED_ENVS"
  exit 1
fi

# ========== Merge ==========
echo ""
echo "========================================"
echo "Merging firmware binaries for M5Burner"
echo "========================================"

# boot_app0.bin — shared across all ESP32 variants
BOOT_APP0="/Users/wilson/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
if [ ! -f "$BOOT_APP0" ]; then
  # Fallback: pick the first variant found
  BOOT_APP0=$(ls /Users/wilson/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin 2>/dev/null | head -1)
fi
if [ ! -f "$BOOT_APP0" ]; then
  echo "Error: boot_app0.bin not found"
  exit 1
fi
echo "Using boot_app0: $BOOT_APP0"

# Environment-specific merge parameters
# Format: chip_type,flash_mode,flash_freq,flash_size,boot_offset
define_env_params() {
  case "$1" in
    m5stack-atoms3)    echo "esp32s3,dio,80m,8MB,0x0000" ;;
    m5stack-sticks3)   echo "esp32s3,dio,80m,8MB,0x0000" ;;
    m5stack-stickcplus) echo "esp32,dio,40m,4MB,0x1000" ;;
    m5stack-cardputer) echo "esp32s3,dio,80m,8MB,0x0000" ;;
    m5stack-m5paper)   echo "esp32,dio,40m,16MB,0x1000" ;;
    *) echo "" ;;
  esac
}

for ENV_NAME in $ENVS; do
  BUILD_DIR="$PROJECT_DIR/.pio/build/$ENV_NAME"

  # Pick an appropriate esptool.py path for this env
  ESPTOOL="esptool.py"
  case "$ENV_NAME" in
    m5stack-sticks3)
      ESPTOOL="/Users/wilson/.platformio/packages/tool-esptoolpy@2.40900.250804/esptool.py"
      ;;
    m5stack-atoms3)
      ESPTOOL="/Users/wilson/.platformio/packages/tool-esptoolpy/esptool.py"
      ;;
    *)
      ESPTOOL="/Users/wilson/.platformio/packages/tool-esptoolpy@1.40501.0/esptool.py"
      ;;
  esac
  if [ ! -f "$ESPTOOL" ]; then
    # Fallback: use whatever is on PATH
    ESPTOOL="esptool.py"
  fi

  PARAMS=$(define_env_params "$ENV_NAME")
  if [ -z "$PARAMS" ]; then
    echo "[SKIP] $ENV_NAME — no merge config defined"
    continue
  fi

  CHIP=$(echo "$PARAMS" | cut -d, -f1)
  FLASH_MODE=$(echo "$PARAMS" | cut -d, -f2)
  FLASH_FREQ=$(echo "$PARAMS" | cut -d, -f3)
  FLASH_SIZE=$(echo "$PARAMS" | cut -d, -f4)
  BOOT_OFFSET=$(echo "$PARAMS" | cut -d, -f5)

  BOOTLOADER="$BUILD_DIR/bootloader.bin"
  PARTITIONS="$BUILD_DIR/partitions.bin"
  FIRMWARE="$BUILD_DIR/firmware.bin"

  if [ ! -f "$BOOTLOADER" ] || [ ! -f "$PARTITIONS" ] || [ ! -f "$FIRMWARE" ]; then
    echo "[SKIP] $ENV_NAME — missing build artifacts (bootloader/partitions/firmware)"
    continue
  fi

  # Friendly platform label for M5Burner
  PLATFORM_LABEL=$(echo "$ENV_NAME" | sed 's/^m5stack-//')
  OUTPUT_FILE="$OUTPUT_DIR/GroveNFC_${PLATFORM_LABEL}_M5Burner_${DATE_TAG}.bin"

  echo ""
  echo "--- Merging $ENV_NAME ---"
  echo "  Chip:        $CHIP"
  echo "  Flash:       ${FLASH_MODE} / ${FLASH_FREQ} / ${FLASH_SIZE}"
  echo "  Boot offset: $BOOT_OFFSET"
  echo "  Output:      $OUTPUT_FILE"

  python3 "$ESPTOOL" --chip "$CHIP" merge_bin \
    -o "$OUTPUT_FILE" \
    --flash_mode "$FLASH_MODE" \
    --flash_freq "$FLASH_FREQ" \
    --flash_size "$FLASH_SIZE" \
    "$BOOT_OFFSET" "$BOOTLOADER" \
    0x8000 "$PARTITIONS" \
    0xe000 "$BOOT_APP0" \
    0x10000 "$FIRMWARE"

  if [ $? -eq 0 ]; then
    echo "[OK] $OUTPUT_FILE"
    shasum -a 256 "$OUTPUT_FILE"
  else
    echo "[FAIL] Merge failed for $ENV_NAME"
    FAILED_ENVS="$FAILED_ENVS $ENV_NAME"
  fi
done

echo ""
echo "========================================"
echo "All merged firmware files are in: $OUTPUT_DIR"
echo "========================================"

if [ -n "$FAILED_ENVS" ]; then
  echo "Failed environments:$FAILED_ENVS"
  exit 1
fi

echo "All done."
