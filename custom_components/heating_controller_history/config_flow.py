"""Config and options flows for Heating Controller."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import callback
from homeassistant.helpers import selector

DOMAIN = "heating_controller_history"

DEFAULT_SETTINGS: dict[str, Any] = {
    "room_count": 3,
    "rooms": [
        {"climate_entity": "climate.nest_thermostat", "humidity_entity": "sensor.living_room_sensor_humidity"},
        {"climate_entity": "climate.shed_fan_thermostat", "humidity_entity": None},
        {"climate_entity": "climate.bedroom_heater", "humidity_entity": "sensor.bedroom_sensor_humidity"},
        {"climate_entity": None, "humidity_entity": None},
    ],
    "outside_temperature_entity": None,
}


def _normalise_input(user_input: dict[str, Any]) -> tuple[dict[str, Any] | None, str | None]:
    """Validate enabled rooms and persist all four editable slots."""
    try:
        room_count = int(user_input.get("room_count", 3))
    except (TypeError, ValueError):
        return None, "invalid_count"

    if room_count < 2 or room_count > 4:
        return None, "invalid_count"

    rooms: list[dict[str, Any]] = []
    enabled_entities: list[str] = []
    for index in range(1, 5):
        climate_entity = user_input.get(f"climate_{index}") or None
        humidity_entity = user_input.get(f"humidity_{index}") or None

        if index <= room_count:
            if not climate_entity:
                return None, "missing_climate"
            if climate_entity in enabled_entities:
                return None, "duplicate_climates"
            enabled_entities.append(climate_entity)

        rooms.append(
            {
                "climate_entity": climate_entity,
                "humidity_entity": humidity_entity,
            }
        )

    return {
        "room_count": room_count,
        "rooms": rooms,
        "outside_temperature_entity": user_input.get("outside_temperature_entity") or None,
    }, None


def _build_schema(current: dict[str, Any]) -> vol.Schema:
    """Build the entity-selector form, prefilled with current choices."""
    current_rooms = current.get("rooms") or DEFAULT_SETTINGS["rooms"]
    schema: dict[Any, Any] = {
        vol.Required(
            "room_count", default=int(current.get("room_count", 3))
        ): selector.NumberSelector(
            {"min": 2, "max": 4, "step": 1, "mode": "box"}
        )
    }

    for index in range(1, 5):
        room = current_rooms[index - 1] if len(current_rooms) >= index else {}
        climate_entity = room.get("climate_entity")
        humidity_entity = room.get("humidity_entity")

        climate_key = (
            vol.Optional(f"climate_{index}", default=climate_entity)
            if climate_entity
            else vol.Optional(f"climate_{index}")
        )
        humidity_key = (
            vol.Optional(f"humidity_{index}", default=humidity_entity)
            if humidity_entity
            else vol.Optional(f"humidity_{index}")
        )
        schema[climate_key] = selector.EntitySelector({"domain": "climate"})
        schema[humidity_key] = selector.EntitySelector({"domain": "sensor"})

    outside_entity = current.get("outside_temperature_entity")
    outside_key = (
        vol.Optional("outside_temperature_entity", default=outside_entity)
        if outside_entity
        else vol.Optional("outside_temperature_entity")
    )
    schema[outside_key] = selector.EntitySelector({"domain": "sensor"})

    return vol.Schema(schema)


class HeatingControllerConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Set up Heating Controller."""

    VERSION = 1

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Create the single Heating Controller config entry."""
        errors: dict[str, str] = {}
        if user_input is not None:
            settings, error = _normalise_input(user_input)
            if error is None and settings is not None:
                await self.async_set_unique_id(DOMAIN)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(
                    title="Heating Controller",
                    data=settings,
                )
            errors["base"] = error or "invalid_count"

        return self.async_show_form(
            step_id="user",
            data_schema=_build_schema(DEFAULT_SETTINGS),
            errors=errors,
        )

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> config_entries.OptionsFlow:
        """Return the options flow for an existing controller."""
        return HeatingControllerOptionsFlow()


class HeatingControllerOptionsFlow(config_entries.OptionsFlow):
    """Allow room assignments to be changed without recompiling firmware."""

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.FlowResult:
        """Edit configured climate rooms and sensors."""
        current = dict(self.config_entry.options or self.config_entry.data)
        errors: dict[str, str] = {}

        if user_input is not None:
            settings, error = _normalise_input(user_input)
            if error is None and settings is not None:
                return self.async_create_entry(title="", data=settings)
            errors["base"] = error or "invalid_count"

        return self.async_show_form(
            step_id="init",
            data_schema=_build_schema(current),
            errors=errors,
        )
