# ClimatePanel

**Smart climate control for Home Assistant**

**Release: 2026.11.25-2**

This release includes the thermostat display setpoint-control improvements:
- Marketplace metadata now includes the explicit optional string fields expected by Home Assistant 2026.11.
- Plus/minus and arc adjustments update locally while the user is interacting.
- Rapid adjustments are debounced; only the final setpoint is sent to Home Assistant.
- Arc dragging is protected against stale Home Assistant refreshes reverting the selected value.
- The display waits for Home Assistant's action response and live climate state confirmation.


ESPHome firmware and the companion Home Assistant integration for the Guition ESP32-S3-4848S040 thermostat display.

> **Breaking change in 2.1.3:** The Home Assistant integration domain is now `climatepanel` (previously `heating_controller_history`). Existing installations must remove the previous integration and add ClimatePanel as a new integration entry; automatic migration is not provided.


## Project contents

- `display-1.yaml` — small ESPHome entry-point template.
- `firmware/packages/` — modular firmware split into hardware, networking, device state, controls, sensors, functional script groups, visual assets and individual LVGL UI packages.
- `custom_components/climatepanel/` — Home Assistant integration. It provides the UI configuration flow, live room data and Recorder history, plus its own integration brand icon.
- `scripts/deploy-thermostat.sh` — validates and deploys the firmware and Home Assistant integration.
- `.github/workflows/validate-esphome.yml` — CI validates the merged, commit-pinned firmware configuration, integration and deployment script.

### Firmware package map

| Area | Package |
|---|---|
| ESP32-S3 board, display, touchscreen and backlight | `firmware/packages/hardware.yaml` |
| Wi-Fi, encrypted API and OTA | `firmware/packages/network.yaml` |
| Globals, clock and periodic runtime | `firmware/packages/state.yaml` |
| Display settings, switches and selectors | `firmware/packages/controls.yaml` |
| Wi-Fi, uptime and Home Assistant sensors | `firmware/packages/sensors.yaml` |
| Display wake, dimming, clock and environment scripts | `firmware/packages/scripts/display.yaml` |
| Climate data, navigation and room synchronisation | `firmware/packages/scripts/climate.yaml` |
| Setpoint and HVAC mode interaction | `firmware/packages/scripts/setpoint.yaml` |
| Dark/light themes and sun-based theme | `firmware/packages/scripts/theme.yaml` |
| Recorder history chart data and rendering | `firmware/packages/scripts/history.yaml` |
| Fonts and rasterised icons | `firmware/packages/assets.yaml` |
| LVGL common setup, thermostat, clock, utility pages and animations | `firmware/packages/ui/` |

The entry point imports these packages from one exact Git commit. Component lists merge by their ESPHome IDs, so scripts, sensors, LVGL pages and animations retain their cross-package references.

Secrets are not stored in this repository. The Home Assistant installation keeps ESPHome credentials in `/config/esphome/secrets.yaml`.

## Climate room configuration

Climate entities are configured in Home Assistant, not in ESPHome substitutions.

After installing the integration, open:

**Settings → Devices & services → ClimatePanel → Configure**

First choose between two and four rooms and, optionally, an outdoor weather entity (recommended) or a legacy temperature sensor. Weather entities supply the current reading through their `temperature` attribute; the integration normalises it to °C for the display. The outdoor reading appears on the dimmed clock screensaver, alongside the indoor temperature and humidity. The next step shows only the climate, humidity and icon selectors for the selected number of rooms. The first room is the Master room and remains the source for the dimmed clock screen's indoor readings.

Settings for disabled rooms are retained. For example, changing from three rooms to two hides the third room's selectors but preserves its climate, humidity and icon selections. Increasing the room count later restores those settings.

Room names come from each climate entity's `friendly_name`. Changes are read by the display automatically; they do not require a GitHub commit, ESPHome YAML edit, firmware compilation or reflash.

The display requests the configuration and current entity states through the ESPHome encrypted Native API response mechanism. Climate controls continue to call Home Assistant's `climate.set_temperature` and `climate.set_hvac_mode` actions for the currently selected entity.

Each room has a configurable icon. The available choices are Home, Living room, Bedroom, Kitchen, Thermostat, Fan, Radiator, Fireplace, Cooling, Office, Server room, Swimming pool, Garage and Air conditioning. The firmware includes these icon assets; changing the selection takes effect without a reflash.

The Home Assistant integration also includes a square brand icon in `custom_components/climatepanel/brand/icon.png`.

## History

The display requests the history action:

```yaml
action: climatepanel.get_climate_history
```

The integration queries Home Assistant Recorder and returns 288 evenly spaced samples:

- Room temperature (`temperatures`)
- Target temperature (`targets`)
- HVAC activity (`activities`: 0 idle/off, 1 heating, 2 cooling, 3 fan/drying)

Recorder reads run through Recorder's dedicated database executor. Recorder must be enabled and retain the requested climate entity's history.

## Diagnostics

The system diagnostics modal shows the Wi-Fi MAC address, IP address, SSID, signal strength and firmware identifier. The identifier is the exact Git tag when the deployed commit is tagged; otherwise it is the first 12 characters of the Git commit SHA. The deployment script generates `/config/esphome/climatepanel-display.yaml` with that identifier and pins every remote package to the exact same full Git commit. It verifies both values before installing the generated entry point. Compile and install **that exact generated file** in the ESPHome Dashboard. Do not compile the repository's `display-1.yaml` directly: it is a template whose defaults are deliberately `local` and `main`. You can independently confirm the deployed values with `grep -E 'build_identifier|firmware_ref' /config/esphome/climatepanel-display.yaml`. The MAC value is synchronised into the diagnostic label after the LVGL interface has been created, so its early Wi-Fi component publication is not lost.

## Home Assistant Marketplace installation

For Home Assistant 2026.11 and newer, the integration can be installed from the Marketplace. Add this repository if it is not already listed:

`https://github.com/AlbertoZen/ClimatePanel`

Select **Integration** as the repository category. After installation, restart Home Assistant. Marketplace installation installs the Home Assistant integration only; it does not compile or flash the ESPHome display firmware. Follow the firmware deployment instructions below for the display.

## Install or update

1. Ensure the integration directory is deployed to `/config/custom_components/climatepanel/`.
2. First update the checkout with `git -C /config/climatepanel pull --ff-only origin main`, then run `/config/climatepanel/scripts/deploy-thermostat.sh` from the Studio Code Server terminal. Pulling first is important: an already-running copy of the previous script cannot execute the newer script version that arrives during its own Git pull. Confirm the deployment output reports the firmware identifier and pinned package commit.
3. In the ESPHome Dashboard, compile and install `/config/esphome/climatepanel-display.yaml` (the generated, commit-pinned entry point, not the Git checkout's template).
4. Restart Home Assistant when the script reports that the integration has changed.
5. On the first migration from the old YAML package, add **ClimatePanel** from **Settings → Devices & services → Add integration**. The initial form is prefilled with the existing three climate entities and their humidity sensors.
6. In **Settings → Devices & services → ESPHome**, open the display device's Configure dialog and ensure it is allowed to perform Home Assistant actions.

The deployment script retires the former `/config/packages/climatepanel.yaml` package and archives it with the existing integration backup. It does not change the top-level packages include in `configuration.yaml`.

The script validates the merged ESPHome configuration against the local secrets and the exact Git commit referenced by the generated entry point. It backs up the previous firmware YAML, retires the old Dashboard entry point, deploys the integration and retires the old Home Assistant YAML package. It does not restart Home Assistant, compile firmware or flash the ESP32. Remote packages are downloaded at build time; the ESP32 does not need GitHub access at runtime.

## Development

Releases use the date-based format `YYYY.MM.DD-REV`, for example `2026.11.25-1`. The revision starts at `1` for the first release on a date and increments for each additional release on that same date. The Home Assistant manifest version and GitHub release tag must always match. The release workflow checks both the date and the next revision before publishing.

Firmware metadata is generated from Git: the deployment script fetches tags, embeds the exact tag when the deployed commit is tagged (otherwise a 12-character commit SHA), and pins all remote packages to the full commit SHA. No manual firmware revision needs to be maintained. The CI workflow resolves and validates the complete modular ESPHome configuration for branch commits and pushed tags; this validates configuration but does not compile or flash firmware.
