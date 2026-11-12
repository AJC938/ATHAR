# ATHAR data protocol (v1)

The wearable unit and the dashboard never talk to each other directly. Both connect to an MQTT broker:

| Side | Connection |
|---|---|
| ESP32-C3 firmware | `broker.hivemq.com`, TCP port **1883** (PubSubClient) |
| Dashboard (browser) | `wss://broker.hivemq.com:8884/mqtt` — automatic fallback to `ws://broker.hivemq.com:8000/mqtt` after 3 s |

> The topic namespace is `positioncare/v1/…` (the project's working name before it became ATHAR). It is kept so that already-flashed devices keep working.

## Topics — `base = positioncare/v1/<DEVICE_ID>`

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `<base>/status` | device → dashboard | yes | status JSON (below), every 1 s and immediately when the alert changes |
| `<base>/online` | device → dashboard | yes | `"1"` after connecting; `"0"` as Last Will |
| `<base>/cmd` | dashboard → device | no | `{"cmd":"reset"}` = patient repositioned · `{"cmd":"ack"}` = mute |

## Status JSON

```json
{
  "v": 1, "id": "pc-4821", "name": "Patient 01", "room": "101",
  "uptime": 123, "still": 95, "stable": 80, "stillReq": 120, "stableReq": 120,
  "moving": false, "motionOk": true, "loaded": true,
  "alert": false, "muted": false, "alerts": 3,
  "zones":  [ {"id": "sacrum", "pct": 45.2, "lvl": 2} ],
  "events": [ {"id": 5, "t": 100, "type": 1} ],
  "th": {"min": 5, "med": 50, "high": 85}
}
```

| Field | Meaning |
|---|---|
| `still` / `stillReq` | seconds without movement / seconds allowed before an alert |
| `stable` / `stableReq` | seconds with unchanged pressure / seconds allowed before an alert |
| `moving` | motion detected since the previous message |
| `motionOk` | `false` if the MPU6050 is not responding (the device then alerts on pressure alone) |
| `loaded` | pressure is present on the sensor |
| `alert` | **no movement AND stable pressure** for the required time |
| `zones[].lvl` | 0 none · 1 low (green) · 2 medium (orange) · 3 high (red) |
| `zones[].id` | `head`, `shoulder_l`, `shoulder_r`, `elbow_l`, `elbow_r`, `sacrum`, `heel_l`, `heel_r` |
| `events[].type` | 1 alert started · 2 cleared by movement / pressure change · 3 reposition confirmed from the dashboard |
| `events[].t` | device uptime (s) when the event happened, newest first |

The dashboard tolerates missing optional fields. If a device sends `still`/`stable` values that never increase, the dashboard computes both timers itself from the motion and pressure readings.

## Device ID

Set `DEVICE_ID` in the firmware and type the same ID in the dashboard (or open `dashboard/index.html?device=<ID>`). The public broker has no authentication, so anyone who knows the ID can read the data — use a private broker with credentials for any real deployment.
