#!/usr/bin/env sh
set -u

# Build all PlatformIO environments defined in platformio.ini.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR="$SCRIPT_DIR"
INI_FILE="$PROJECT_DIR/platformio.ini"

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

FAILED_ENVS=""
TOTAL=0
SUCCESS=0

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

echo "All environments built successfully."
