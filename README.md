# Drone-in-a-Box

Smart Solutions Semester Fall 2026–2027 project: a mobile drone-in-a-box system
mounted on a vehicle roof, able to safely deploy, recover, center and transport a
small drone.

## Repository layout

```
docs/                      project documentation
  visual-representation.md  project phases as a Mermaid flowchart
  architecture/             how the parts talk to each other
  images/                   diagrams exported as PNG
  reference/                supplied reference material (mechanical drawings, datasheets)
  private/                  not committed — contracts, agreements, anything non-public
firmware/                  ESP32-S3 firmware
  drone_box/                ESP-IDF project: linear actuator over serial (current)
  actuator_test/            original Arduino sketch, superseded by drone_box
```

## Documentation

- [Project phases and scope](docs/visual-representation.md) — diagnosis, objectives,
  solution direction.
- Phase diagrams: [diagnosis](docs/images/1-diagnosis.png),
  [goal](docs/images/2-goal.png), [solution](docs/images/3-solution.png).
- [Vehicle dock drawing](docs/reference/vehicle-dock-1.pdf).
- [Control interface](docs/architecture/control-interface.md) — how a phone, an
  app or a serial cable reaches the dock, the HTTP API contract, and where it
  goes in production.

## Firmware

Current work is the ESP-IDF project in **[firmware/drone_box](firmware/drone_box)**
— see [its README](firmware/drone_box/README.md) for wiring, a code walkthrough and
the bench test cases. Target: ESP32-S3, ESP-IDF v6.0.2.

```bash
. ~/.espressif/tools/activate_idf_v6.0.2.sh
cd firmware/drone_box && idf.py flash monitor
```

The `.ino` sketches are the Arduino originals it was ported from, kept for reference.

Serial monitor at **115200 baud**. For both:

| Key | Action |
| --- | --- |
| `e` | extend |
| `r` | retract |
| `s` | stop |
| `1`–`9` | speed 10 %–90 % |
| `0` | full speed |

Wiring: `GPIO4 → D2`, `GPIO5 → D3`, ESP32 `GND` → driver `GND`.
