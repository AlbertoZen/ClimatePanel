# Changelog

## 0.2.1
- Fixed LVGL flag operations for the heating/cooling activity band containers.


## 0.2.0
- One climate entity per tile; use Tessera pages for room navigation.
- Removed the room navigation arrows and the in-tile history link. Tapping the dial opens history.
- Added 1-hour, 24-hour and 1-week history selection.
- Added recorded heating/cooling activity bands from the climate entity's historical `hvac_action`.
- Reworked the dial and the temperature typography to match the standalone ESPHome display.
- Replaced HVAC mode text with Tessera icon-font glyphs and enlarged the setpoint keys.

## 0.1.1
- Enlarged the thermostat dial and moved the current temperature and status inside it.
- Replaced the unsupported minus glyph with a standard hyphen.
- Fixed Recorder history field paths to match the direct `history/history_during_period` response.
- Requested the full recorded climate history for the chart.

## 0.1.0
- First beta version of the multi-room thermostat tile and 24-hour temperature history card.
- Uses Tessera entity selection, Home Assistant climate actions and Recorder history.
