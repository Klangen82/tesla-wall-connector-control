"""Exercise the HA helper's freshness decisions without requiring a running HA."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
from types import SimpleNamespace

import jinja2
import yaml

NOW = datetime(2026, 10, 10, tzinfo=timezone.utc)
ENTITY_IDS = [f"sensor.your_phase{phase}_current" for phase in (1, 2, 3)]


class States(dict):
    def __call__(self, entity_id):
        return self[entity_id].state if entity_id in self else "unknown"


def as_timestamp(value, default=0):
    try:
        return value.timestamp()
    except (AttributeError, ValueError, TypeError):
        return default


def is_number(value):
    try:
        import math
        return math.isfinite(float(value))
    except (ValueError, TypeError):
        return False


source = yaml.safe_load(
    (Path(__file__).parent.parent / "examples/ha-house-meter-reporting.yaml").read_text()
)["template"][0]["binary_sensor"][0]["state"]
env = jinja2.Environment(undefined=jinja2.StrictUndefined)
env.globals.update(now=lambda: NOW, as_timestamp=as_timestamp, is_number=is_number)
template = env.from_string(source)


def healthy_states():
    return States({entity: SimpleNamespace(
        state="1.0", last_reported=NOW - timedelta(seconds=5),
        last_changed=NOW - timedelta(days=1),
    ) for entity in ENTITY_IDS})


def evaluate(states):
    return template.render(states=states).strip() == "True"


assert evaluate(healthy_states())  # Constant current with recent reports is healthy.
for bad_state in ("unknown", "unavailable", "nan", "inf", "not a number"):
    states = healthy_states()
    states[ENTITY_IDS[1]].state = bad_state
    assert not evaluate(states)
states = healthy_states()
del states[ENTITY_IDS[2]]
assert not evaluate(states)
for age in (45, 1000, -1):
    states = healthy_states()
    states[ENTITY_IDS[0]].last_reported = NOW - timedelta(seconds=age)
    assert not evaluate(states)
states = healthy_states()
states[ENTITY_IDS[0]].last_reported = NOW - timedelta(seconds=44.9)
assert evaluate(states)
print("All house reporting helper scenarios passed")
