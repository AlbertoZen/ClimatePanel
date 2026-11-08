# Thermostat ESPHome project

Source repository for the Heating Controller display firmware and its companion Home Assistant history integration.

## Project contents

- `display-1.yaml` — ESPHome firmware configuration for the Guition ESP32-S3-4848S040 display.
- `custom_components/heating_controller_history/` — Home Assistant custom integration that reads climate history from Recorder and returns chart data to the display.
- `packages/heating_controller_history.yaml` — YAML package that loads the history integration.
- `.github/workflows/validate-esphome.yml` — CI validation for the ESPHome configuration and companion integration.

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

The deployment script backs up the active YAML and validates it against the local secrets before replacing it. It does not compile or flash firmware.

The history integration is Home Assistant-side code. The deployment script currently deploys only `display-1.yaml`; updating the integration requires copying its directory and package to the Home Assistant paths listed above, then restarting Home Assistant.

## Validation

GitHub Actions runs on pushes and pull requests. It checks the Python syntax and metadata/YAML files for the history integration, then runs `esphome config display-1.yaml` using temporary CI-only placeholder secrets.

CI does not contain or access the real Home Assistant secrets, and it does not build or flash firmware.
