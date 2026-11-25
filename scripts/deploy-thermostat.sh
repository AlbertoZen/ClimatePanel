#!/usr/bin/env bash
set -Eeuo pipefail

REPO="/config/climatepanel"
HA_CONFIG="/config"
ESPHOME_DIR="$HA_CONFIG/esphome"
SECRETS="$ESPHOME_DIR/secrets.yaml"
FIRMWARE_SOURCE="$REPO/display-1.yaml"
FIRMWARE_TARGET="$ESPHOME_DIR/display-1.yaml"
COMPONENT_SOURCE="$REPO/custom_components/climatepanel"
COMPONENT_TARGET="$HA_CONFIG/custom_components/climatepanel"
PACKAGES_DIR="$HA_CONFIG/packages"
PACKAGE_TARGET="$PACKAGES_DIR/climatepanel.yaml"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"

FIRMWARE_CANDIDATE=""
VALIDATION_LOG=""
COMPONENT_STAGE=""
COMPONENT_OLD=""
PACKAGE_OLD=""
COMPONENT_INSTALLED=false
PACKAGE_REMOVED=false

cleanup() {
  [[ -n "$FIRMWARE_CANDIDATE" && -f "$FIRMWARE_CANDIDATE" ]] && rm -f "$FIRMWARE_CANDIDATE"
  [[ -n "$VALIDATION_LOG" && -f "$VALIDATION_LOG" ]] && rm -f "$VALIDATION_LOG"
  [[ -n "$COMPONENT_STAGE" && -d "$COMPONENT_STAGE" ]] && rm -rf "$COMPONENT_STAGE"
  return 0
}
trap cleanup EXIT

fail() { echo "ERROR: $*" >&2; exit 1; }

restore_bridge() {
  if [[ "$PACKAGE_REMOVED" == true && -n "$PACKAGE_OLD" && -f "$PACKAGE_OLD" ]]; then
    mv "$PACKAGE_OLD" "$PACKAGE_TARGET"
    PACKAGE_REMOVED=false
  fi
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
echo "   CLIMATEPANEL + HOME ASSISTANT CONFIG"
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
git -C "$REPO" fetch --tags origin
git -C "$REPO" pull --ff-only origin main

if BUILD_IDENTIFIER="$(git -C "$REPO" describe --tags --exact-match HEAD 2>/dev/null)"; then
  echo "  Firmware identifier: release tag $BUILD_IDENTIFIER"
else
  BUILD_IDENTIFIER="$(git -C "$REPO" rev-parse --short=12 HEAD)"
  echo "  Firmware identifier: Git commit $BUILD_IDENTIFIER"
fi
echo "  Prepared firmware config: $FIRMWARE_TARGET"

[[ -f "$FIRMWARE_SOURCE" ]] || fail "Firmware source is missing."
[[ -f "$COMPONENT_SOURCE/__init__.py" ]] || fail "History integration __init__.py is missing."
[[ -f "$COMPONENT_SOURCE/manifest.json" ]] || fail "History integration manifest.json is missing."
[[ -f "$COMPONENT_SOURCE/services.yaml" ]] || fail "History integration services.yaml is missing."

echo
echo "2. Checking local secrets..."
for secret in display_1__encryption_password wifi_ssid wifi_password; do
  grep -Eq "^${secret}[[:space:]]*:" "$SECRETS" || fail "Required local secret is missing: $secret"
  echo "  OK: $secret"
done

echo
echo "3. Checking Home Assistant integration files..."
python3 - "$COMPONENT_SOURCE" <<'PY'
import ast
import json
import sys
from pathlib import Path

component = Path(sys.argv[1])
for source_file in (component / "__init__.py", component / "config_flow.py"):
    ast.parse(source_file.read_text(), filename=str(source_file))

manifest = json.loads((component / "manifest.json").read_text())
if manifest.get("domain") != "climatepanel":
    raise SystemExit("Integration manifest domain does not match its folder")
if manifest.get("config_flow") is not True:
    raise SystemExit("ClimatePanel must expose a config flow")
if "recorder" not in manifest.get("dependencies", []):
    raise SystemExit("Integration must declare its Recorder dependency")

try:
    import yaml
    services = yaml.safe_load((component / "services.yaml").read_text()) or {}
except ImportError:
    from ruamel.yaml import YAML
    services = YAML(typ="safe").load((component / "services.yaml").read_text()) or {}

required = {"get_display_data", "get_climate_history"}
if not required.issubset(services):
    raise SystemExit("One or more ClimatePanel actions are missing")
source = (component / "__init__.py").read_text()
if "async_setup_entry" not in source or "async_register" not in source:
    raise SystemExit("Config-entry service registration was not found")
print("  Python syntax, manifest, config flow and services: OK")
PY

echo
echo "4. Validating ESPHome configuration..."
FIRMWARE_CANDIDATE="$(mktemp "$ESPHOME_DIR/.display-1-candidate.XXXXXX.yaml")"
VALIDATION_LOG="$(mktemp /tmp/thermostat-validation.XXXXXX.log)"
cp "$FIRMWARE_SOURCE" "$FIRMWARE_CANDIDATE"
chmod --reference="$FIRMWARE_TARGET" "$FIRMWARE_CANDIDATE"
python3 - "$FIRMWARE_CANDIDATE" "$BUILD_IDENTIFIER" <<'PY'
import json
import sys
from pathlib import Path

firmware_path = Path(sys.argv[1])
build_identifier = sys.argv[2]
firmware = firmware_path.read_text()
placeholder = '  build_identifier: "local"'
if firmware.count(placeholder) != 1:
    raise SystemExit("Expected exactly one build_identifier placeholder in display-1.yaml")
firmware_path.write_text(
    firmware.replace(placeholder, f"  build_identifier: {json.dumps(build_identifier)}")
)
PY
python3 - "$FIRMWARE_CANDIDATE" "$BUILD_IDENTIFIER" <<'PY'
import sys
from pathlib import Path

firmware = Path(sys.argv[1]).read_text()
expected = f'  build_identifier: "{sys.argv[2]}"'
if expected not in firmware:
    raise SystemExit("Generated firmware config does not contain the expected Git identifier")
print(f"  Embedded firmware identifier: {sys.argv[2]}")
PY
if ! esphome config "$FIRMWARE_CANDIDATE" >"$VALIDATION_LOG" 2>&1; then
  echo "ESPHome validation failed. Active firmware has NOT been changed."
  show_validation_error
  exit 1
fi
echo "  ESPHome configuration: OK"

FIRMWARE_CHANGED=false
COMPONENT_CHANGED=false
PACKAGE_CHANGED=false
cmp -s "$FIRMWARE_CANDIDATE" "$FIRMWARE_TARGET" || FIRMWARE_CHANGED=true
if [[ ! -d "$COMPONENT_TARGET" ]] || ! diff -qr "$COMPONENT_SOURCE" "$COMPONENT_TARGET" >/dev/null 2>&1; then
  COMPONENT_CHANGED=true
fi
if [[ -f "$PACKAGE_TARGET" ]]; then
  # The integration is moving from YAML package setup to a Home Assistant config entry.
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
  COMPONENT_STAGE="$(mktemp -d "$HA_CONFIG/custom_components/.climatepanel.stage.XXXXXX")"
  cp -a "$COMPONENT_SOURCE/." "$COMPONENT_STAGE/"
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
  [[ -d "$COMPONENT_TARGET" ]] && BACKUP_ITEMS+=(custom_components/climatepanel)
  [[ -f "$PACKAGE_TARGET" ]] && BACKUP_ITEMS+=(packages/climatepanel.yaml)
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
    COMPONENT_OLD="$HA_CONFIG/custom_components/.climatepanel.previous.$STAMP"
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
  PACKAGE_OLD="$PACKAGES_DIR/.climatepanel.yaml.retired.$STAMP"
  if ! mv "$PACKAGE_TARGET" "$PACKAGE_OLD"; then
    echo "Could not retire the legacy YAML package; restoring previous integration."
    restore_bridge
    fail "Could not retire the legacy history integration package."
  fi
  PACKAGE_REMOVED=true
fi

if [[ "$FIRMWARE_CHANGED" == true ]]; then
  if ! mv -f "$FIRMWARE_CANDIDATE" "$FIRMWARE_TARGET"; then
    echo "Firmware replacement failed; restoring previous history integration."
    restore_bridge
    fail "Could not replace active firmware YAML."
  fi
  FIRMWARE_CANDIDATE=""
fi

EXPECTED_BUILD_IDENTIFIER_LINE="  build_identifier: \"$BUILD_IDENTIFIER\""
if ! grep -Fqx "$EXPECTED_BUILD_IDENTIFIER_LINE" "$FIRMWARE_TARGET"; then
  fail "Installed firmware YAML does not contain the expected Git identifier: $BUILD_IDENTIFIER"
fi
echo "  Installed firmware identifier: $BUILD_IDENTIFIER"
echo "  Compile this exact file in ESPHome Dashboard: $FIRMWARE_TARGET"

[[ -n "$COMPONENT_OLD" && -d "$COMPONENT_OLD" ]] && rm -rf "$COMPONENT_OLD"
[[ -n "$PACKAGE_OLD" && -f "$PACKAGE_OLD" ]] && rm -f "$PACKAGE_OLD"

PACKAGE_LOADER_FOUND=false
if [[ -f "$HA_CONFIG/configuration.yaml" ]] && python3 - "$HA_CONFIG/configuration.yaml" <<'PY'
from pathlib import Path
import re
import sys

configuration = Path(sys.argv[1]).read_text(errors="replace")
for raw_line in configuration.splitlines():
    # Strip comments and normalise whitespace/CRLF before matching.
    line = raw_line.split("#", 1)[0].strip()
    if re.fullmatch(
        r"packages:\s*!include_dir_(?:named|merge_named)\s+(?:\./)?packages",
        line,
    ):
        raise SystemExit(0)
raise SystemExit(1)
PY
then
  PACKAGE_LOADER_FOUND=true
fi

echo
echo "======================================"
echo "       DEPLOYMENT COMPLETED"
echo "======================================"
[[ "$FIRMWARE_CHANGED" == true ]] && echo "Firmware YAML: updated (ESPHome validated)" || echo "Firmware YAML: already current"
[[ "$COMPONENT_CHANGED" == true ]] && echo "ClimatePanel integration: updated" || echo "ClimatePanel integration: already current"
[[ "$PACKAGE_CHANGED" == true ]] && echo "Legacy YAML package: retired" || echo "Legacy YAML package: not present"
echo
if [[ "$BRIDGE_CHANGED" == true ]]; then
  echo "Restart Home Assistant to load the config-entry-based ClimatePanel integration."
  echo "The existing packages include in configuration.yaml is left unchanged."
  echo "After restart, add 'ClimatePanel' from Settings > Devices & services > Add integration."
  echo "The first setup form is prefilled with the current three room assignments."
else
  echo "No Home Assistant integration restart is required."
fi
echo "Compile and install $FIRMWARE_TARGET in the ESPHome Dashboard."
echo "Firmware was NOT compiled or flashed."
echo "======================================"
