"""Expose Recorder climate history as a response-capable Home Assistant action.

The ESPHome display calls this service over the encrypted Native API. No REST
endpoint or Home Assistant access token is used.
"""

from __future__ import annotations

import logging
import math
from datetime import timedelta
from functools import partial
from typing import Any

import voluptuous as vol

from homeassistant.components.recorder.history import get_significant_states
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import config_validation as cv
from homeassistant.util import dt as dt_util

DOMAIN = "heating_controller_history"
_LOGGER = logging.getLogger(__name__)
SERVICE_GET_CLIMATE_HISTORY = "get_climate_history"
POINT_COUNT = 288

# This integration is configured from YAML with no options.
# Home Assistant requires a CONFIG_SCHEMA for integrations defining async_setup().
CONFIG_SCHEMA = cv.empty_config_schema(DOMAIN)

SERVICE_SCHEMA = vol.Schema(
    {
        vol.Required("entity_id"): cv.entity_id,
        vol.Optional("period_hours", default=24): vol.All(
            vol.Coerce(int), vol.Range(min=1, max=24)
        ),
    }
)


def _number(value: Any) -> float | None:
    """Convert a Recorder attribute to a finite float, if possible."""
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def _state_attributes(state: Any) -> dict[str, Any]:
    """Return state attributes as a plain dictionary."""
    attributes = getattr(state, "attributes", None)
    return dict(attributes) if attributes is not None else {}


async def async_setup(hass: HomeAssistant, config: dict[str, Any]) -> bool:
    """Register the Recorder history action."""
    if DOMAIN not in config:
        return True

    _LOGGER.info("Setting up Heating Controller History bridge")

    async def async_get_climate_history(call: ServiceCall) -> dict[str, Any]:
        entity_id = call.data["entity_id"]
        _LOGGER.debug("History request received for %s", entity_id)
        if not entity_id.startswith("climate."):
            raise HomeAssistantError("entity_id must refer to a climate entity")

        period_hours = int(call.data.get("period_hours", 24))
        if period_hours < 1 or period_hours > 24:
            raise HomeAssistantError("period_hours must be between 1 and 24")

        end_time = dt_util.utcnow()
        start_time = end_time - timedelta(hours=period_hours)

        # The Recorder query is synchronous and must run in Home Assistant's
        # executor, not on the event loop. Request full attributes and all state
        # updates so current_temperature, setpoint and hvac_action are retained.
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

        # Build an evenly spaced, step-held graph. For a 24-hour period this is
        # one point every five minutes, matching the existing 288-point chart.
        interval_seconds = period_hours * 3600.0 / POINT_COUNT
        temperatures: list[float] = []
        targets: list[float] = []
        activities: list[int] = []
        valid_count = 0
        state_index = 0
        current_state = None

        for point in range(POINT_COUNT):
            sample_time = start_time + timedelta(seconds=interval_seconds * point)
            while state_index < len(states) and states[state_index].last_updated <= sample_time:
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
        SERVICE_GET_CLIMATE_HISTORY,
        async_get_climate_history,
        schema=SERVICE_SCHEMA,
        supports_response=SupportsResponse.ONLY,
    )
    _LOGGER.info("Registered action %s.%s", DOMAIN, SERVICE_GET_CLIMATE_HISTORY)
    return True
