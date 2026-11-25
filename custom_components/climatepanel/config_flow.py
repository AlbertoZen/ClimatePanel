"""Config and options flows for ClimatePanel."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import callback
from homeassistant.helpers import selector

DOMAIN = "climatepanel"

ICON_OPTIONS = [
    {"label": "Home", "value": "home"},
    {"label": "Living room", "value": "sofa"},
    {"label": "Bedroom", "value": "bed"},
    {"label": "Kitchen", "value": "chef-hat"},
    {"label": "Thermostat", "value": "thermostat"},
    {"label": "Fan", "value": "fan"},
    {"label": "Radiator", "value": "radiator"},
    {"label": "Fireplace", "value": "fire"},
    {"label": "Cooling", "value": "snowflake"},
    {"label": "Office", "value": "desk"},
    {"label": "Server room", "value": "server"},
    {"label": "Swimming pool", "value": "pool"},
    {"label": "Garage", "value": "garage"},
    {"label": "Air conditioning", "value": "air-conditioner"},
]
DEFAULT_ICONS = ["home", "chef-hat", "bed", "thermostat"]

DEFAULT_SETTINGS: dict[str, Any] = {
    "room_count": 3,
    "rooms": [
        {
            "climate_entity": "climate.nest_thermostat",
            "humidity_entity": "sensor.living_room_sensor_humidity",
            "icon": "home",
        },
        {
            "climate_entity": "climate.shed_fan_thermostat",
            "humidity_entity": None,
            "icon": "chef-hat",
        },
        {
            "climate_entity": "climate.bedroom_heater",
            "humidity_entity": "sensor.bedroom_sensor_humidity",
            "icon": "bed",
        },
        {
            "climate_entity": None,
            "humidity_entity": None,
            "icon": "thermostat",
        },
    ],
    "outside_temperature_entity": None,
}


def _build_count_schema(current: dict[str, Any]) -> vol.Schema:
    """Ask for the room count before displaying the room-specific fields."""
    schema: dict[Any, Any] = {
        vol.Required(
            "room_count", default=int(current.get("room_count", 3))
        ): selector.NumberSelector(
            {"min": 2, "max": 4, "step": 1, "mode": "box"}
        )
    }

    outside_entity = current.get("outside_temperature_entity")
    outside_key = (
        vol.Optional("outside_temperature_entity", default=outside_entity)
        if outside_entity
        else vol.Optional("outside_temperature_entity")
    )
    # Weather entities expose the current temperature as an attribute. Keep
    # sensor entities selectable so existing installations remain compatible.
    schema[outside_key] = selector.EntitySelector({"domain": ["weather", "sensor"]})
    return vol.Schema(schema)


def _build_room_schema(current: dict[str, Any], room_count: int) -> vol.Schema:
    """Build selectors only for the enabled rooms."""
    current_rooms = current.get("rooms") or DEFAULT_SETTINGS["rooms"]
    schema: dict[Any, Any] = {}

    for index in range(1, room_count + 1):
        room = current_rooms[index - 1] if len(current_rooms) >= index else {}
        climate_entity = room.get("climate_entity")
        humidity_entity = room.get("humidity_entity")
        icon = room.get("icon") or DEFAULT_ICONS[index - 1]

        climate_key = (
            vol.Required(f"climate_{index}", default=climate_entity)
            if climate_entity
            else vol.Required(f"climate_{index}")
        )
        humidity_key = (
            vol.Optional(f"humidity_{index}", default=humidity_entity)
            if humidity_entity
            else vol.Optional(f"humidity_{index}")
        )
        schema[climate_key] = selector.EntitySelector({"domain": "climate"})
        schema[humidity_key] = selector.EntitySelector({"domain": "sensor"})
        schema[vol.Required(f"icon_{index}", default=icon)] = (
            selector.SelectSelector(
                {"options": ICON_OPTIONS, "mode": "dropdown"}
            )
        )

    return vol.Schema(schema)


def _normalise_room_input(
    current: dict[str, Any],
    room_count: int,
    user_input: dict[str, Any],
) -> tuple[dict[str, Any] | None, str | None]:
    """Merge visible room fields into the saved settings and validate them."""
    if room_count < 2 or room_count > 4:
        return None, "invalid_count"

    existing_rooms = current.get("rooms") or DEFAULT_SETTINGS["rooms"]
    rooms = [
        dict(existing_rooms[index]) if index < len(existing_rooms) else {
            "climate_entity": None,
            "humidity_entity": None,
            "icon": DEFAULT_ICONS[index],
        }
        for index in range(4)
    ]
    enabled_entities: list[str] = []
    supported_icons = {option["value"] for option in ICON_OPTIONS}

    for index in range(1, room_count + 1):
        room = rooms[index - 1]
        climate_key = f"climate_{index}"
        humidity_key = f"humidity_{index}"
        icon_key = f"icon_{index}"

        climate_entity = user_input.get(climate_key) or room.get("climate_entity")
        # An omitted optional humidity selector means the user left it empty.
        humidity_entity = user_input.get(humidity_key)
        icon = (
            user_input.get(icon_key)
            or room.get("icon")
            or DEFAULT_ICONS[index - 1]
        )

        if not climate_entity:
            return None, "missing_climate"
        if climate_entity in enabled_entities:
            return None, "duplicate_climates"
        if icon not in supported_icons:
            return None, "invalid_icon"

        enabled_entities.append(climate_entity)
        room.update(
            {
                "climate_entity": climate_entity,
                "humidity_entity": humidity_entity or None,
                "icon": icon,
            }
        )

    settings = dict(current)
    settings.update({"room_count": room_count, "rooms": rooms})
    return settings, None


class ConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Set up ClimatePanel."""

    VERSION = 1

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Select the room count and then show only the required room slots."""
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                room_count = int(user_input.get("room_count", 3))
            except (TypeError, ValueError):
                room_count = 0

            if room_count < 2 or room_count > 4:
                errors["base"] = "invalid_count"
            else:
                outside_entity = (
                    user_input.get("outside_temperature_entity")
                    if "outside_temperature_entity" in user_input
                    else DEFAULT_SETTINGS.get("outside_temperature_entity")
                )
                self._pending_settings = {
                    **DEFAULT_SETTINGS,
                    "room_count": room_count,
                    "rooms": [dict(room) for room in DEFAULT_SETTINGS["rooms"]],
                    "outside_temperature_entity": outside_entity or None,
                }
                return await self.async_step_rooms()

        return self.async_show_form(
            step_id="user",
            data_schema=_build_count_schema(DEFAULT_SETTINGS),
            errors=errors,
        )

    async def async_step_rooms(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Configure only the enabled rooms."""
        current = getattr(self, "_pending_settings", DEFAULT_SETTINGS)
        room_count = int(current.get("room_count", 3))
        errors: dict[str, str] = {}

        if user_input is not None:
            settings, error = _normalise_room_input(
                current, room_count, user_input
            )
            if settings is not None and error is None:
                await self.async_set_unique_id(DOMAIN)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(
                    title="ClimatePanel",
                    data=settings,
                )
            errors["base"] = error or "invalid_count"

        return self.async_show_form(
            step_id="rooms",
            data_schema=_build_room_schema(current, room_count),
            errors=errors,
        )

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> config_entries.OptionsFlow:
        """Return the options flow for an existing controller."""
        return ClimatePanelOptionsFlow()


class ClimatePanelOptionsFlow(config_entries.OptionsFlow):
    """Allow room assignments to be changed without recompiling firmware."""

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.FlowResult:
        """Choose the number of visible room slots."""
        current = dict(self.config_entry.options or self.config_entry.data)
        errors: dict[str, str] = {}

        if user_input is not None:
            try:
                room_count = int(user_input.get("room_count", 3))
            except (TypeError, ValueError):
                room_count = 0

            if room_count < 2 or room_count > 4:
                errors["base"] = "invalid_count"
            else:
                outside_entity = (
                    user_input.get("outside_temperature_entity")
                    if "outside_temperature_entity" in user_input
                    else current.get("outside_temperature_entity")
                )
                self._pending_settings = {
                    **current,
                    "room_count": room_count,
                    "rooms": [
                        dict(room)
                        for room in (
                            current.get("rooms") or DEFAULT_SETTINGS["rooms"]
                        )
                    ],
                    "outside_temperature_entity": outside_entity or None,
                }
                return await self.async_step_rooms()

        return self.async_show_form(
            step_id="init",
            data_schema=_build_count_schema(current),
            errors=errors,
        )

    async def async_step_rooms(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.FlowResult:
        """Edit the enabled room assignments and retain disabled room settings."""
        current = getattr(
            self,
            "_pending_settings",
            dict(self.config_entry.options or self.config_entry.data),
        )
        room_count = int(current.get("room_count", 3))
        errors: dict[str, str] = {}

        if user_input is not None:
            settings, error = _normalise_room_input(
                current, room_count, user_input
            )
            if settings is not None and error is None:
                return self.async_create_entry(title="", data=settings)
            errors["base"] = error or "invalid_count"

        return self.async_show_form(
            step_id="rooms",
            data_schema=_build_room_schema(current, room_count),
            errors=errors,
        )
