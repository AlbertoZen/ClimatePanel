"""Heating Controller integration.

Provides dynamic climate configuration and Recorder history to the ESPHome
thermostat display over the encrypted Native API response mechanism.
"""

from __future__ import annotations

import logging
import math
from datetime import timedelta
from functools import partial
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry
from homeassistant.components.recorder.history import get_significant_states
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse
from homeassistant.exceptions import HomeAssistantError
from homeassistant.util import dt as dt_util

DOMAIN = "heating_controller_history"
_LOGGER = logging.getLogger(__name__)

SERVICE_GET_CLIMATE_HISTORY = "get_climate_history"
SERVICE_GET_DISPLAY_DATA = "get_display_data"
POINT_COUNT = 288

HISTORY_SERVICE_SCHEMA = vol.Schema(
    {
        vol.Required("entity_id"): vol.All(str, vol.Length(min=1)),
        vol.Optional("period_hours", default=24): vol.All(
            vol.Coerce(int), vol.Range(min=1, max=24)
        ),
    }
)

DISPLAY_SERVICE_SCHEMA = vol.Schema({})


def _number(value: Any) -> float | None:
    """Convert a state or Recorder attribute to a finite float."""
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def _state_attributes(state: Any) -> dict[str, Any]:
    """Return state attributes as a plain dictionary."""
    attributes = getattr(state, "attributes", None)
    return dict(attributes) if attributes is not None else {}


def _entry_settings(entry: ConfigEntry) -> dict[str, Any]:
    """Return the latest editable settings, falling back to initial setup data."""
    return dict(entry.options or entry.data)


def _climate_payload(hass: HomeAssistant, room: dict[str, Any]) -> dict[str, Any]:
    """Build the current display payload for one configured room."""
    entity_id = str(room["climate_entity"])
    climate_state = hass.states.get(entity_id)
    attributes = _state_attributes(climate_state)

    humidity_value = None
    humidity_entity = room.get("humidity_entity")
    if humidity_entity:
        humidity_state = hass.states.get(str(humidity_entity))
        if humidity_state is not None:
            humidity_value = _number(humidity_state.state)

    supported_modes = attributes.get("hvac_modes", [])
    if not isinstance(supported_modes, (list, tuple)):
        supported_modes = []

    return {
        "entity_id": entity_id,
        "name": attributes.get("friendly_name") or entity_id.split(".", 1)[-1],
        "icon": str(room.get("icon") or "home"),
        "current_temperature": _number(attributes.get("current_temperature")),
        "target_temperature": _number(attributes.get("temperature")),
        "target_temp_low": _number(attributes.get("target_temp_low")),
        "target_temp_high": _number(attributes.get("target_temp_high")),
        "humidity": humidity_value,
        "supported_modes": [str(mode) for mode in supported_modes],
        "hvac_mode": str(climate_state.state) if climate_state is not None else "",
        "hvac_action": str(attributes.get("hvac_action", "")),
    }


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up the Heating Controller services for one config entry."""
    _LOGGER.info("Setting up Heating Controller")

    async def async_get_display_data(_call: ServiceCall) -> dict[str, Any]:
        """Return the current configured rooms and live entity states."""
        settings = _entry_settings(entry)
        room_count = int(settings.get("room_count", 3))
        configured_rooms = settings.get("rooms", [])

        if room_count < 2 or room_count > 4 or len(configured_rooms) < room_count:
            raise HomeAssistantError(
                "Heating Controller room configuration is incomplete"
            )

        rooms: list[dict[str, Any]] = []
        for room in configured_rooms[:room_count]:
            climate_entity = room.get("climate_entity")
            if not climate_entity:
                raise HomeAssistantError(
                    "Every enabled room must have a climate entity selected"
                )
            rooms.append(_climate_payload(hass, room))

        outside_temperature = None
        outside_entity = settings.get("outside_temperature_entity")
        if outside_entity:
            outside_state = hass.states.get(str(outside_entity))
            if outside_state is not None:
                outside_temperature = _number(outside_state.state)

        return {
            "room_count": room_count,
            "rooms": rooms,
            "outside_temperature": outside_temperature,
        }

    async def async_get_climate_history(call: ServiceCall) -> dict[str, Any]:
        """Return evenly sampled climate history from Recorder."""
        entity_id = call.data["entity_id"]
        _LOGGER.debug("History request received for %s", entity_id)
        if not entity_id.startswith("climate."):
            raise HomeAssistantError("entity_id must refer to a climate entity")

        period_hours = int(call.data.get("period_hours", 24))
        if period_hours < 1 or period_hours > 24:
            raise HomeAssistantError("period_hours must be between 1 and 24")

        end_time = dt_util.utcnow()
        start_time = end_time - timedelta(hours=period_hours)

        # Recorder queries are synchronous and must run in Home Assistant's
        # executor, not on the event loop.
        query = partial(
            get_significant_states,
            hass,
            start_time,
            end_time,
            entity_ids=[entity_id],
            include_start_time_state=True,
            significant_changes_only=False,
            minimal_response=False,
            no_attributes=False,
            compressed_state_format=False,
        )
        states_by_entity = await hass.async_add_executor_job(query)
        states = list(states_by_entity.get(entity_id, []))
        states.sort(key=lambda item: item.last_updated)

        interval_seconds = period_hours * 3600.0 / POINT_COUNT
        temperatures: list[float] = []
        targets: list[float] = []
        activities: list[int] = []
        valid_count = 0
        state_index = 0
        current_state = None

        for point in range(POINT_COUNT):
            sample_time = start_time + timedelta(seconds=interval_seconds * point)
            while (
                state_index < len(states)
                and states[state_index].last_updated <= sample_time
            ):
                current_state = states[state_index]
                state_index += 1

            if current_state is None:
                temperatures.append(-100.0)
                targets.append(-100.0)
                activities.append(0)
                continue

            attributes = _state_attributes(current_state)
            room_temperature = _number(attributes.get("current_temperature"))

            target_temperature = _number(attributes.get("temperature"))
            if target_temperature is None:
                low = _number(attributes.get("target_temp_low"))
                high = _number(attributes.get("target_temp_high"))
                if low is not None and high is not None:
                    target_temperature = (low + high) / 2.0

            if room_temperature is None:
                temperatures.append(-100.0)
            else:
                temperatures.append(room_temperature)
                valid_count += 1

            targets.append(
                target_temperature if target_temperature is not None else -100.0
            )

            hvac_action = str(attributes.get("hvac_action", "")).lower()
            climate_state = str(getattr(current_state, "state", "")).lower()
            if climate_state == "off":
                activity = 0
            elif hvac_action == "heating":
                activity = 1
            elif hvac_action == "cooling":
                activity = 2
            elif hvac_action in ("fan", "drying"):
                activity = 3
            else:
                activity = 0
            activities.append(activity)

        if valid_count:
            status_text = (
                f"Home Assistant Recorder | last {period_hours}h | "
                f"{valid_count}/{POINT_COUNT} temperature points"
            )
        else:
            status_text = "No recorded temperature history found for this climate"

        return {
            "temperatures": temperatures,
            "targets": targets,
            "activities": activities,
            "period_hours": period_hours,
            "valid_count": valid_count,
            "status_text": status_text,
        }

    hass.services.async_register(
        DOMAIN,
        SERVICE_GET_DISPLAY_DATA,
        async_get_display_data,
        schema=DISPLAY_SERVICE_SCHEMA,
        supports_response=SupportsResponse.ONLY,
    )
    hass.services.async_register(
        DOMAIN,
        SERVICE_GET_CLIMATE_HISTORY,
        async_get_climate_history,
        schema=HISTORY_SERVICE_SCHEMA,
        supports_response=SupportsResponse.ONLY,
    )
    _LOGGER.info(
        "Registered actions %s.%s and %s.%s",
        DOMAIN,
        SERVICE_GET_DISPLAY_DATA,
        DOMAIN,
        SERVICE_GET_CLIMATE_HISTORY,
    )
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload the integration services."""
    hass.services.async_remove(DOMAIN, SERVICE_GET_DISPLAY_DATA)
    hass.services.async_remove(DOMAIN, SERVICE_GET_CLIMATE_HISTORY)
    return True
