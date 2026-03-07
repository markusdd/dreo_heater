# Dreo Heater ESPHome Component

This is an **external component for ESPHome** to control Dreo wall heaters using ESPHome custom firmware. It is designed for BK72xx devices such as the BK7231N Tuya platform.

## Features
- Full thermostat/climate integration
- Control sound, display, child lock, window mode
- Manual heat level and timer support
- Temperature calibration

## Structure
- `dreo_heater/` — ESPHome component code (Python + C++)
- `example.yaml` — Example ESPHome config for your heater

## Installation (as External Component)

Add to your ESPHome YAML:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/lygris/dreo_heater.git
    refresh: 0s
```

Then use the `climate` platform in your config (see example.yaml).

## Example YAML
See [`example.yaml`](example.yaml) or below:

```yaml
# ... platform and WiFi setup ...
external_components:
  - source:
      type: git
      url: https://github.com/lygris/dreo_heater.git
    refresh: 0s

uart:
  id: tuya_uart
  tx_pin: TX1
  rx_pin: RX1
  baud_rate: 115200

climate:
  - platform: dreo_heater
    id: my_dreo
    uart_id: tuya_uart
    name: "Dreo Heater"

# ... more options ...
```

## Usage
- Many advanced options—such as sound, display, child lock, window detection, heat level, timer, and calibration—are available and shown in the [`example.yaml`](example.yaml). Consult this file to see all configurable switches and features you can add to your configuration.
- See comments in `example.yaml` for additional tips on customizing these options.
- Use with devices using BK7231N chips (Tuya-based Dreo wall heaters, etc)

## Contributing
PRs welcome! Open an issue or pull request if you improve compatibility or add features.

## License
[MIT](LICENSE) (add this if you want to specify one)
