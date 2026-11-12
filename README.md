# Heating Controller

**Release: 2026.11.09**

This release includes the thermostat display setpoint-control improvements:
- Plus/minus and arc adjustments update locally while the user is interacting.
- Rapid adjustments are debounced; only the final setpoint is sent to Home Assistant.
- Arc dragging is protected against stale Home Assistant refreshes reverting the selected value.
- The display waits for Home Assistant's action response and live climate state confirmation.


ESPHome firmware and the companion Home Assistant integration for the Guition ESP32-S3-4848S040 thermostat display.

## Project contents

- `display-1.yaml` — ESPHome display firmware.
- `custom_components/heating_controller_history/` — Home Assistant integration. It provides the UI configuration flow, live room data and Recorder history, plus its own integration brand icon.
- `scripts/deploy-thermostat.sh` — validates and deploys the firmware and Home Assistant integration.
- `.github/workflows/validate-esphome.yml` — CI checks for the firmware, integration and deployment script.

Secrets are not stored in this repository. The Home Assistant installation keeps ESPHome credentials in `/config/esphome/secrets.yaml`.

## Climate room configuration

Climate entities are configured in Home Assistant, not in ESPHome substitutions.

After installing the integration, open:

**Settings → Devices & services → Heating Controller → Configure**

First choose between two and four rooms and, optionally, an outdoor weather entity (recommended) or a legacy temperature sensor. Weather entities supply the current reading through their `temperature` attribute; the integration normalises it to °C for the display. The outdoor reading appears on the dimmed clock screensaver, alongside the indoor temperature and humidity. The next step shows only the climate, humidity and icon selectors for the selected number of rooms. The first room is the Master room and remains the source for the dimmed clock screen's indoor readings.

Settings for disabled rooms are retained. For example, changing from three rooms to two hides the third room's selectors but preserves its climate, humidity and icon selections. Increasing the room count later restores those settings.

Room names come from each climate entity's `friendly_name`. Changes are read by the display automatically; they do not require a GitHub commit, ESPHome YAML edit, firmware compilation or reflash.

The display requests the configuration and current entity states through the ESPHome encrypted Native API response mechanism. Climate controls continue to call Home Assistant's `climate.set_temperature` and `climate.set_hvac_mode` actions for the currently selected entity.

Each room has a configurable icon. The available choices are Home, Living room, Bedroom, Kitchen, Thermostat, Fan, Radiator, Fireplace, Cooling, Office, Server room, Swimming pool, Garage and Air conditioning. The firmware includes these icon assets; changing the selection takes effect without a reflash.

The Home Assistant integration also includes a square brand icon in `custom_components/heating_controller_history/brand/icon.png`.

## History

The display requests the history action:

```yaml
action: heating_controller_history.get_climate_history
```

The integration queries Home Assistant Recorder and returns 288 evenly spaced samples:

- Room temperature (`temperatures`)
- Target temperature (`targets`)
- HVAC activity (`activities`: 0 idle/off, 1 heating, 2 cooling, 3 fan/drying)

Recorder reads run through Recorder's dedicated database executor. Recorder must be enabled and retain the requested climate entity's history.

## Diagnostics

The system diagnostics modal shows the Wi-Fi MAC address, IP address, SSID, signal strength and firmware identifier. The identifier is the exact Git tag when the deployed commit is tagged; otherwise it is the first 12 characters of the Git commit SHA. The deployment script embeds this value in `/config/esphome/display-1.yaml`. Compile and install **that file** in the ESPHome Dashboard after running the script. Do not compile `/config/thermostat-esphome/display-1.yaml` from the Git checkout directly: that source file deliberately retains the `local` placeholder. The MAC value is synchronised into the diagnostic label after the LVGL interface has been created, so its early Wi-Fi component publication is not lost.

## Install or update

1. Ensure the integration directory is deployed to `/config/custom_components/heating_controller_history/`.
2. Run `/config/thermostat-esphome/scripts/deploy-thermostat.sh` from the Studio Code Server terminal so the current Git-managed deployment script is used. Confirm its output reports a tag or 12-character commit SHA.
3. In the ESPHome Dashboard, compile and install `/config/esphome/display-1.yaml` (the deployed copy, not the Git checkout's source YAML).
4. Restart Home Assistant when the script reports that the integration has changed.
5. On the first migration from the old YAML package, add **Heating Controller** from **Settings → Devices & services → Add integration**. The initial form is prefilled with the existing three climate entities and their humidity sensors.
6. In **Settings → Devices & services → ESPHome**, open the display device's Configure dialog and ensure it is allowed to perform Home Assistant actions.

The deployment script retires the former `/config/packages/heating_controller_history.yaml` package and archives it with the existing integration backup. It does not change the top-level packages include in `configuration.yaml`.

The script validates the ESPHome YAML against the local secrets, backs up the active firmware YAML, deploys the integration and retires the old package. It does not restart Home Assistant, compile firmware or flash the ESP32.

## Development

Firmware metadata is generated from Git: the deployment script fetches tags and embeds the exact tag when the deployed commit is tagged, otherwise a 12-character commit SHA. No manual firmware revision needs to be maintained. The CI workflow validates the ESPHome configuration for branch commits and pushed tags; this validates configuration but does not compile or flash firmware.
