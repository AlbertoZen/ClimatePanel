# Heating Controller for Tessera

A multi-room thermostat interface for the Guition ESP32-S3-4848S040 4-inch display.

The plugin is a native Tessera extension. It uses Tessera's entity selectors and live state updates, Home Assistant climate actions, and Home Assistant's own Recorder history command. It does not install or require the standalone Heating Controller Home Assistant integration.

## What it provides

- A dedicated thermostat tile for two to four climate entities.
- Linear room navigation, with the first configured climate as the Master room.
- Current temperature, setpoint, HVAC action and available HVAC mode controls.
- Setpoint adjustment with an optimistic display and a short debounce before sending the final value.
- Dual setpoints (target_temp_low and target_temp_high) where supplied by the climate entity.
- Optional humidity sensors for each room and an optional outdoor temperature or weather entity.
- An optional current-temperature dot on the setpoint arc.
- A custom 24-hour temperature history card with room and target temperature curves.

Tessera remains responsible for display configuration, brightness, standby, screensavers, diagnostics, navigation, themes, Wi-Fi, and firmware updates.

## Set up

This repository keeps the standalone firmware on main. The Tessera plugin is developed on the tessera-plugin branch, in the tessera-plugin/ folder.

In Tessera, open **Plugins → Add with a link** and use:

https://github.com/AlbertoZen/thermostat-esphome/tree/tessera-plugin/tessera-plugin

The plugin repository is private. If Tessera cannot retrieve it through the GitHub link, use the local-folder method described in Tessera's [plugin development guide](https://github.com/MaxGramser/tessera-plugins/blob/main/docs/MAKING_A_PLUGIN.md#9-put-it-on-your-screen): copy the contents of this `tessera-plugin/` directory into `/config/tessera-plugins/heating_controller/`, next to the `esphome` folder.

Add the plugin to the Guition screen. Then open Layout, add the **Heating Controller** tile from Plugins, and choose the climate entities and optional sensors in the inspector.

On the Guition's default **2 columns × 3 rows** grid, the tile can occupy the whole page. You can also change the grid to **3 columns × 5 rows** and resize the tile to cover that page. The minimum tile size is 2 × 3 cells so the main controls remain usable.

## History

The history card requests Home Assistant's history/history_during_period command in four six-hour intervals. This keeps each request bounded by Tessera's response-size limit while retaining the climate entity's Recorder attributes.

Version 0.1 displays the room-temperature and target-temperature curves. The previous standalone chart's HVAC activity bands are not included yet: Tessera's bounded answer mapper currently handles compact numeric arrays, but not a parallel list of categorical HVAC states. The graph uses the climate entity's history directly; no additional Recorder integration is installed.

## Deliberate first-version limitations

- Room icons are not independently configurable yet; the display uses room names from the selected entities.
- The selected room starts at the Master room after a reboot.
- HVAC activity bands, the standalone clock, diagnostics and display-idle controls are delegated to Tessera or deferred.
- A Home Assistant action being sent is not proof that it completed. The tile keeps the optimistic setpoint briefly and reconciles it against subsequent entity updates.

## Development

The manifest targets plugin API 0.24, matching the current Tessera release API. The plugin is Guition-only for this first iteration.

From a checkout of MaxGramser/tessera-plugins, Tessera's own checker can validate this folder with:

python3 tools/check.py /path/to/tessera-plugin

For the plugin preview and physical testing, follow Tessera's docs/TESTING.md and docs/PREVIEW.md.

No physical Guition hardware test or firmware flash is implied by this source commit.
