# Thermostat ESPHome project

Source repository for the Heating Controller display firmware and its companion Home Assistant history integration.

## Project contents

- `display-1.yaml` — ESPHome firmware configuration for the Guition ESP32-S3-4848S040 display.
- `custom_components/heating_controller_history/` — Home Assistant custom integration that reads climate history from Recorder and returns chart data to the display.
- `packages/heating_controller_history.yaml` — YAML package that loads the history integration.
- `scripts/deploy-thermostat.sh` — combined deployment script for the ESPHome firmware and Home Assistant history integration.
- `.github/workflows/validate-esphome.yml` — CI validation for the deployment script, ESPHome configuration and companion integration.

Secrets are deliberately not stored in this repository. The active Home Assistant installation keeps its credentials in `/config/esphome/secrets.yaml`.

## Heating Controller History integration

The display requests the history action:

```yaml
action: heating_controller_history.get_climate_history
```

The integration queries Home Assistant Recorder for the selected climate entity and returns 288 evenly spaced samples containing:

- Room temperature (`temperatures`)
- Target temperature (`targets`)
- HVAC activity (`activities`: 0 idle/off, 1 heating, 2 cooling, 3 fan/drying)

The action is exposed over the ESPHome encrypted Native API response mechanism. It does not use a REST endpoint, long-lived access token, or HTTP request.

### Install or update in Home Assistant

Copy the integration directory into the Home Assistant configuration directory so the final layout is:

```text
/config/custom_components/heating_controller_history/
  __init__.py
  manifest.json
  services.yaml
```

Copy the package file to:

```text
/config/packages/heating_controller_history.yaml
```

The package assumes Home Assistant packages are enabled. If they are not already enabled, merge this into the existing top-level `homeassistant:` section in `configuration.yaml` (do not create a duplicate section):

```yaml
homeassistant:
  packages: !include_dir_named packages
```

The package loads the integration with an empty configuration:

```yaml
heating_controller_history: {}
```

After copying or updating the integration:

1. Run Home Assistant configuration validation.
2. Restart Home Assistant fully; reloading YAML alone does not load a newly added Python integration.
3. Check the logs for `Setting up Heating Controller History bridge` and `Registered action heating_controller_history.get_climate_history`.
4. In Settings → Devices & services → ESPHome, open the Heating Controller device's Configure dialog and allow it to perform Home Assistant actions.

Recorder must be enabled and retain the requested entity history. The integration supports climate entities and periods from 1 to 24 hours.

## Development and deployment

Edit the source in GitHub, validate via GitHub Actions, then deploy the firmware YAML to the ESPHome configuration directory with:

```bash
/config/deploy-thermostat.sh
```

The version-controlled deployment script is `scripts/deploy-thermostat.sh`. Install it at `/config/deploy-thermostat.sh` and run it from the Home Assistant Studio Code Server terminal.

The script:
- Pulls the latest `main` branch using fast-forward-only Git updates.
- Checks the required local secrets without displaying their values.
- Checks the Home Assistant integration's Python syntax, manifest, service description and package YAML.
- Validates the firmware against the local `secrets.yaml` before replacing the active YAML.
- Backs up the firmware separately under `/config/esphome/backups/`.
- Archives the previous history integration and package separately under `/config/backups/thermostat-history-bridge/`.
- Deploys the integration to `/config/custom_components/heating_controller_history/` and the package to `/config/packages/heating_controller_history.yaml`.
- Uses staged replacements and attempts to restore the previous integration/package if a replacement step fails.

It does not restart Home Assistant, compile firmware or flash the ESP32. When the integration or package changes, a full Home Assistant restart is required before the new Python integration is loaded. The script warns if it cannot recognise the normal packages include in `configuration.yaml`; it does not modify that file automatically.

## Validation

GitHub Actions runs on pushes and pull requests. It checks the Python syntax and metadata/YAML files for the history integration, then runs `esphome config display-1.yaml` using temporary CI-only placeholder secrets.

CI does not contain or access the real Home Assistant secrets, and it does not build or flash firmware.
