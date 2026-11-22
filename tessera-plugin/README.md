# Heating Controller for Tessera

A climate tile for the Guition ESP32-S3-4848S040 4-inch display.

The plugin is a native Tessera extension. It uses Tessera's climate entity selection and live state updates, Home Assistant climate actions, and Home Assistant's Recorder history command. It does not install or require the standalone Heating Controller Home Assistant integration.

## What it provides

- One thermostat tile for each climate entity. Add separate tiles to Tessera pages for Master, Kitchen, Bedroom, or any other climate entity.
- A large setpoint dial, with the setpoint's whole number, decimal and unit arranged to match the standalone ESPHome display.
- Large minus and plus controls for reliable setpoint adjustment, with a short debounce and optimistic display.
- HVAC mode controls drawn as icons from Tessera's icon font.
- Current temperature, HVAC action, optional humidity and optional outdoor temperature.
- Optional current-temperature marker on the setpoint arc.
- Tap the dial to open the temperature history.
- History periods of 1 hour, 24 hours and 1 week.
- Room temperature and target temperature curves, with shaded bands for recorded heating and cooling activity.

Tessera remains responsible for display configuration, brightness, standby, screensavers, diagnostics, page navigation, themes, Wi-Fi and firmware updates.

## Set up

The repository is private, so Tessera cannot retrieve it from the GitHub link directly. Use the local plugin folder:

1. Copy the **contents** of this `tessera-plugin/` directory into `/config/tessera-plugins/heating_controller/`, next to the `esphome` directory.
2. Check that `tessera-plugin.yaml` is directly inside `/config/tessera-plugins/heating_controller/`.
3. In Tessera, add the Heating Controller plugin to your Guition screen.
4. Open **Layout**, add a Heating Controller tile, and select the climate entity that tile should control.
5. Add another tile for each other room. Use Tessera pages to organise the rooms; the plugin itself does not have room-navigation arrows.

You can also download the branch ZIP while signed in to GitHub:
https://github.com/AlbertoZen/thermostat-esphome/archive/refs/heads/tessera-plugin.zip

## Controls

The large minus and plus buttons adjust the selected climate entity's setpoint. For a dual-target climate, tap the low or high target label first to select which end the buttons adjust.

Tap inside the dial to open the history card. The HVAC mode icons remain below the dial and send `climate.set_hvac_mode` when tapped.

The plugin uses Tessera's tile tap API. That API does not expose the native climate card's drag or hold gestures, so setpoint adjustment is through the large on-screen minus and plus controls rather than dragging the arc.

## History

The card provides 1h, 24h and 1w period selectors. The graph shows room temperature and target temperature, with amber bands for heating and blue bands for cooling. These activity bands are derived from the climate entity's historical `hvac_action` attribute; the Home Assistant Recorder must retain those climate states and attributes for the selected period.

To stay within Tessera's bounded Home Assistant response size while retaining the climate attributes needed for activity, the plugin requests history in one-hour slices. The 1-week view therefore makes more requests and may take longer to finish loading than the shorter periods.

No additional Recorder integration is installed. The history comes from Home Assistant's existing `history/history_during_period` websocket command.

## Development notes

- Plugin API target: 0.24.
- Board: Guition.
- The standalone firmware remains on the repository's `main` branch; this plugin is developed on `tessera-plugin`.
- Tessera's official checker validates the manifest, translations, folder layout and static plugin rules. It does not compile the complete ESPHome firmware or flash the screen.
- A full firmware build and physical Guition test are still required after installing each plugin revision.

For Tessera's local testing instructions, see the [plugin development guide](https://github.com/MaxGramser/tessera-plugins/blob/main/docs/MAKING_A_PLUGIN.md#9-put-it-on-your-screen).
