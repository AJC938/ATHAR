# Wokwi simulation

A browser simulation of the wearable unit, used to develop and test the dashboard before the hardware was built. It needs **no external libraries** — a minimal MQTT client is included inside `sketch.ino`.

## Files to add in a new Wokwi ESP32-C3 project

| Wokwi file | Source |
|---|---|
| `sketch.ino` | `wokwi/sketch.ino` |
| `fsr.chip.json` + `fsr.chip.c` | custom FSR chip (a slider that simulates pressure) |

## Wiring (XIAO ESP32-C3 in Wokwi)

| Part | Pin | Board |
|---|---|---|
| MPU6050 | VCC / GND | 3V3 / GND |
| MPU6050 | SDA | D8 (GPIO8) |
| MPU6050 | SCL | D9 (GPIO9) |
| FSR chip | VCC / GND | 3V3 / GND |
| FSR chip | OUT | D2 (GPIO4) |
| Buzzer (optional) | + | D3 (GPIO5) |

## Run

1. Set your own `DEVICE_ID` at the top of `sketch.ino` (the broker is public). Wi-Fi is already `Wokwi-GUEST`.
2. Press ▶ — the serial monitor prints `[WiFi] connected` and `[MQTT] connected`.
3. Open `dashboard/index.html?device=<your ID>`. Move the FSR slider to change the pressure map; tilt the MPU6050 to simulate movement.
