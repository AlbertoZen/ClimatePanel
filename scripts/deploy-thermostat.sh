#!/usr/bin/env bash
set -Eeuo pipefail

REPO="/config/thermostat-esphome"
HA_CONFIG="/config"
ESPHOME_DIR="$HA_CONFIG/esphome"
SECRETS="$ESPHOME_DIR/secrets.yaml"
FIRMWARE_SOURCE="$REPO/display-1.yaml"
FIRMWARE_TARGET="$ESPHOME_DIR/display-1.yaml"
COMPONENT_SOURCE="$REPO/custom_components/heating_controller_history"
COMPONENT_TARGET="$HA_CONFIG/custom_components/heating_controller_history"
PACKAGE_SOURCE="$REPO/packages/heating_controller_history.yaml"
PACKAGES_DIR="$HA_CONFIG/packages"
PACKAGE_TARGET="$PACKAGES_DIR/heating_controller_history.yaml"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"

FIRMWARE_CANDIDATE=""
VALIDATION_LOG=""
COMPONENT_STAGE=""
PACKAGE_STAGE=""
COMPONENT_OLD=""
PACKAGE_OLD=""
COMPONENT_INSTALLED=false
PACKAGE_INSTALLED=false

cleanup() {
  [[ -n "$FIRMWARE_CANDIDATE" && -f "$FIRMWARE_CANDIDATE" ]] && rm -f "$FIRMWARE_CANDIDATE"
  [[ -n "$VALIDATION_LOG" && -f "$VALIDATION_LOG" ]] && rm -f "$VALIDATION_LOG"
  [[ -n "$COMPONENT_STAGE" && -d "$COMPONENT_STAGE" ]] && rm -rf "$COMPONENT_STAGE"
  [[ -n "$PACKAGE_STAGE" && -f "$PACKAGE_STAGE" ]] && rm -f "$PACKAGE_STAGE"
  return 0
}
trap cleanup EXIT

fail() { echo "ERROR: $*" >&2; exit 1; }

restore_bridge() {
  if [[ "$PACKAGE_INSTALLED" == true ]]; then rm -f "$PACKAGE_TARGET"; fi
  if [[ -n "$PACKAGE_OLD" && -f "$PACKAGE_OLD" ]]; then mv "$PACKAGE_OLD" "$PACKAGE_TARGET"; fi
  if [[ "$COMPONENT_INSTALLED" == true ]]; then rm -rf "$COMPONENT_TARGET"; fi
  if [[ -n "$COMPONENT_OLD" && -d "$COMPONENT_OLD" ]]; then mv "$COMPONENT_OLD" "$COMPONENT_TARGET"; fi
}

show_validation_error() {
  python3 - "$VALIDATION_LOG" "$SECRETS" <<'PY'
from pathlib import Path
import sys
log_path, secrets_path = map(Path, sys.argv[1:])
text = log_path.read_text(errors="replace")
for line in secrets_path.read_text(errors="replace").splitlines():
    if ":" not in line or line.lstrip().startswith("#"):
        continue
    value = line.split(":", 1)[1].strip().strip("\"'")
    if len(value) >= 4:
        text = text.replace(value, "[REDACTED]")
print("\n".join(text.splitlines()[-45:]))
PY
}

echo "======================================"
echo "   THERMOSTAT + HISTORY DEPLOYMENT"
echo "======================================"

for cmd in git esphome python3 tar diff cmp; do
  command -v "$cmd" >/dev/null 2>&1 || fail "$cmd is not installed."
done

[[ -d "$REPO/.git" ]] || fail "Git repository not found: $REPO"
[[ -f "$FIRMWARE_TARGET" ]] || fail "Active thermostat YAML not found: $FIRMWARE_TARGET"
[[ -f "$SECRETS" ]] || fail "Local secrets.yaml not found."

if [[ -n "$(git -C "$REPO" status --porcelain)" ]]; then
  fail "GitHub working copy contains uncommitted changes. Commit or discard them first."
fi
if git -C "$REPO" ls-files --error-unmatch secrets.yaml >/dev/null 2>&1; then
  fail "secrets.yaml is tracked by Git. Remove it from the repository before deploying."
fi

echo
echo "1. Updating source from GitHub..."
git -C "$REPO" pull --ff-only origin main

[[ -f "$FIRMWARE_SOURCE" ]] || fail "Firmware source is missing."
[[ -f "$COMPONENT_SOURCE/__init__.py" ]] || fail "History integration __init__.py is missing."
[[ -f "$COMPONENT_SOURCE/manifest.json" ]] || fail "History integration manifest.json is missing."
[[ -f "$COMPONENT_SOURCE/services.yaml" ]] || fail "History integration services.yaml is missing."
[[ -f "$PACKAGE_SOURCE" ]] || fail "History integration package is missing."

echo
echo "2. Checking local secrets..."
for secret in display_1__encryption_password wifi_ssid wifi_password; do
  grep -Eq "^\${secret}[[:space:]]*:" "$SECRETS" || fail "Required local secret is missing: $secret"
  echo "  OK: $secret"
done

echo
echo "3. Checking Home Assistant integration files..."
python3 - "$COMPONENT_SOURCE" "$PACKAGE_SOURCE" <<'PY'
import ast
import json
import sys
from pathlib import Path

component = Path(sys.argv[1])
package_file = Path(sys.argv[2])
source = (component / "__init__.py").read_text()
ast.parse(source, filename=str(component / "__init__.py"))
manifest = json.loads((component / "manifest.json").read_text())
if manifest.get("domain") != "heating_controller_history":
    raise SystemExit("Integration manifest domain does not match its folder")
if "recorder" not in manifest.get("dependencies", []):
    raise SystemExit("Integration manifest must declare its Recorder dependency")

try:
    import yaml
    load_yaml = yaml.safe_load
except ImportError:
    from ruamel.yaml import YAML
    parser = YAML(typ="safe")
    load_yaml = parser.load

services = load_yaml((component / "services.yaml").read_text()) or {}
if "get_climate_history" not in services:
    raise SystemExit("get_climate_history is missing from services.yaml")
package = load_yaml(package_file.read_text()) or {}
if package.get("heating_controller_history") != {}:
    raise SystemExit("Package must load heating_controller_history with an empty mapping")
if "get_climate_history" not in source or "async_register" not in source:
    raise SystemExit("History action registration was not found in __init__.py")
print("  Python syntax, manifest, services and package: OK")
PY

echo
echo "4. Validating ESPHome configuration..."
FIRMWARE_CANDIDATE="$(mktemp "$ESPHOME_DIR/.display-1-candidate.XXXXXX.yaml")"
VALIDATION_LOG="$(mktemp /tmp/thermostat-validation.XXXXXX.log)"
cp "$FIRMWARE_SOURCE" "$FIRMWARE_CANDIDATE"
chmod --reference="$FIRMWARE_TARGET" "$FIRMWARE_CANDIDATE"
if ! esphome config "$FIRMWARE_CANDIDATE" >"$VALIDATION_LOG" 2>&1; then
  echo "ESPHome validation failed. Active firmware has NOT been changed."
  show_validation_error
  exit 1
fi
echo "  ESPHome configuration: OK"

FIRMWARE_CHANGED=false
COMPONENT_CHANGED=false
PACKAGE_CHANGED=false
cmp -s "$FIRMWARE_SOURCE" "$FIRMWARE_TARGET" || FIRMWARE_CHANGED=true
if [[ ! -d "$COMPONENT_TARGET" ]] || ! diff -qr "$COMPONENT_SOURCE" "$COMPONENT_TARGET" >/dev/null 2>&1; then
  COMPONENT_CHANGED=true
fi
if [[ ! -f "$PACKAGE_TARGET" ]] || ! cmp -s "$PACKAGE_SOURCE" "$PACKAGE_TARGET"; then
  PACKAGE_CHANGED=true
fi
BRIDGE_CHANGED=false
if [[ "$COMPONENT_CHANGED" == true || "$PACKAGE_CHANGED" == true ]]; then
  BRIDGE_CHANGED=true
fi

if [[ "$FIRMWARE_CHANGED" == false && "$BRIDGE_CHANGED" == false ]]; then
  echo
  echo "Everything is already up to date. No files changed; no restart required."
  exit 0
fi

[[ ! -L "$FIRMWARE_TARGET" ]] || fail "Active firmware YAML is a symlink; refusing to replace it."
[[ ! -L "$COMPONENT_TARGET" ]] || fail "History integration destination is a symlink; refusing to replace it."
[[ ! -L "$PACKAGE_TARGET" ]] || fail "History package destination is a symlink; refusing to replace it."
[[ ! -e "$COMPONENT_TARGET" || -d "$COMPONENT_TARGET" ]] || fail "History integration destination exists but is not a directory."
[[ ! -e "$PACKAGE_TARGET" || -f "$PACKAGE_TARGET" ]] || fail "History package destination exists but is not a regular file."

mkdir -p "$ESPHOME_DIR/backups" "$HA_CONFIG/backups/thermostat-history-bridge" "$HA_CONFIG/custom_components" "$PACKAGES_DIR"

if [[ "$COMPONENT_CHANGED" == true ]]; then
  COMPONENT_STAGE="$(mktemp -d "$HA_CONFIG/custom_components/.heating_controller_history.stage.XXXXXX")"
  cp -a "$COMPONENT_SOURCE/." "$COMPONENT_STAGE/"
fi
if [[ "$PACKAGE_CHANGED" == true ]]; then
  PACKAGE_STAGE="$(mktemp "$PACKAGES_DIR/.heating_controller_history.yaml.stage.XXXXXX")"
  cp -p "$PACKAGE_SOURCE" "$PACKAGE_STAGE"
fi

echo
echo "5. Creating backups..."
if [[ "$FIRMWARE_CHANGED" == true ]]; then
  FIRMWARE_BACKUP="$ESPHOME_DIR/backups/display-1-$STAMP.yaml"
  cp -p "$FIRMWARE_TARGET" "$FIRMWARE_BACKUP"
  echo "  Firmware backup: $FIRMWARE_BACKUP"
fi

if [[ "$BRIDGE_CHANGED" == true ]]; then
  BACKUP_ITEMS=()
  [[ -d "$COMPONENT_TARGET" ]] && BACKUP_ITEMS+=(custom_components/heating_controller_history)
  [[ -f "$PACKAGE_TARGET" ]] && BACKUP_ITEMS+=(packages/heating_controller_history.yaml)
  if (( ${#BACKUP_ITEMS[@]} > 0 )); then
    BRIDGE_BACKUP="$HA_CONFIG/backups/thermostat-history-bridge/history-bridge-$STAMP.tar.gz"
    tar -czf "$BRIDGE_BACKUP" -C "$HA_CONFIG" "${BACKUP_ITEMS[@]}"
    echo "  History bridge backup: $BRIDGE_BACKUP"
  else
    echo "  No previous history integration/package existed to back up."
  fi
fi

if [[ "$COMPONENT_CHANGED" == true ]]; then
  if [[ -e "$COMPONENT_TARGET" ]]; then
    COMPONENT_OLD="$HA_CONFIG/custom_components/.heating_controller_history.previous.$STAMP"
    mv "$COMPONENT_TARGET" "$COMPONENT_OLD"
  fi
  if ! mv "$COMPONENT_STAGE" "$COMPONENT_TARGET"; then
    [[ -n "$COMPONENT_OLD" && -d "$COMPONENT_OLD" ]] && mv "$COMPONENT_OLD" "$COMPONENT_TARGET"
    fail "Could not install the history integration directory."
  fi
  COMPONENT_STAGE=""
  COMPONENT_INSTALLED=true
fi

if [[ "$PACKAGE_CHANGED" == true ]]; then
  if [[ -e "$PACKAGE_TARGET" ]]; then
    PACKAGE_OLD="$PACKAGES_DIR/.heating_controller_history.yaml.previous.$STAMP"
    mv "$PACKAGE_TARGET" "$PACKAGE_OLD"
  fi
  if ! mv "$PACKAGE_STAGE" "$PACKAGE_TARGET"; then
    echo "Package replacement failed; restoring previous history integration."
    restore_bridge
    fail "Could not install the history integration package."
  fi
  PACKAGE_STAGE=""
  PACKAGE_INSTALLED=true
fi

if [[ "$FIRMWARE_CHANGED" == true ]]; then
  if ! mv -f "$FIRMWARE_CANDIDATE" "$FIRMWARE_TARGET"; then
    echo "Firmware replacement failed; restoring previous history integration."
    restore_bridge
    fail "Could not replace active firmware YAML."
  fi
  FIRMWARE_CANDIDATE=""
fi

[[ -n "$COMPONENT_OLD" && -d "$COMPONENT_OLD" ]] && rm -rf "$COMPONENT_OLD"
[[ -n "$PACKAGE_OLD" && -f "$PACKAGE_OLD" ]] && rm -f "$PACKAGE_OLD"

PACKAGE_LOADER_FOUND=false
if [[ -f "$HA_CONFIG/configuration.yaml" ]] && \
   grep -Ev '^[[:space:]]*#|^[[:space:]]*$' "$HA_CONFIG/configuration.yaml" | \
   grep -Eq '^[[:space:]]*packages:[[:space:]]*!include_dir_(named|merge_named)[[:space:]]+(\./)?packages([[:space:]]*(#.*)?$)'; then
  PACKAGE_LOADER_FOUND=true
fi

echo
echo "======================================"
echo "       DEPLOYMENT COMPLETED"
echo "======================================"
[[ "$FIRMWARE_CHANGED" == true ]] && echo "Firmware YAML: updated (ESPHome validated)" || echo "Firmware YAML: already current"
[[ "$COMPONENT_CHANGED" == true ]] && echo "History integration: updated" || echo "History integration: already current"
[[ "$PACKAGE_CHANGED" == true ]] && echo "History package: updated" || echo "History package: already current"
echo
if [[ "$BRIDGE_CHANGED" == true ]]; then
  echo "ACTION REQUIRED: Restart Home Assistant to load/update the custom integration."
  if [[ "$PACKAGE_LOADER_FOUND" == false ]]; then
    echo "WARNING: Could not confirm packages are enabled in configuration.yaml."
    echo "Ensure the existing top-level homeassistant: section includes:"
    echo "  packages: !include_dir_named packages"
    echo "Do not create a duplicate homeassistant: section."
  else
    echo "Home Assistant packages include appears to be configured."
  fi
  echo "The script has NOT restarted Home Assistant."
else
  echo "No Home Assistant integration restart is required."
fi
echo "Firmware was NOT compiled or flashed."
echo "======================================"
