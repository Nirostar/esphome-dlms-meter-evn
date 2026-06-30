# esphome-dlms-meter-evn

An [ESPHome](https://esphome.io/) **external component** for reading
encrypted DLMS smart-meter data from an Austrian
[Kaifa MA309M](https://www.kaifa-meter.com/) (as deployed by *EVN / Netz
Niederösterreich*) over its M-Bus customer interface.

Forked from [webeandr/esphome-dlms-meter](https://github.com/webeandr/esphome-dlms-meter)
(an EVN-special fork of [DomiStyle/esphome-dlms-meter](https://github.com/DomiStyle/esphome-dlms-meter))
and modernized to work with **ESPHome ≥ 2026.6**, which removed the legacy
`custom_component` API.

## What changed vs. the original

| Aspect | Original | This fork |
|---|---|---|
| Integration mechanism | `custom_component:` lambda + `esphome.includes:` | First-class `external_components` with a declarative YAML schema |
| Sensor wiring | C++ pointers inside a lambda | Plain YAML: `voltage: { l1: ..., l2: ..., l3: ... }` |
| Decryption key | C++ byte array literal | YAML hex string (`!secret`-friendly) |
| AES-GCM on ESP32 | `mbedtls_gcm_*` (broken in ESP-IDF 5.x where `MBEDTLS_GCM_ALT` is on) | ESP-IDF `esp_aes_gcm_*` — **hardware-accelerated** |
| Component header includes | `#include "esphome.h"` (no longer permitted in external components) | Specific per-feature ESPHome headers |
| MQTT support | Always compiled | Guarded by `#ifdef USE_MQTT` |
| ESPHome compatibility | ≤ 2026.5 | ≥ 2026.6 (tested on 2026.6.3 with ESP-IDF 5.5) |

## Supported hardware

* **Meter:** Kaifa MA309M (EVN / Netz Niederösterreich)
* **MCU:** any ESPHome-supported ESP32 (incl. C3 / C6 / S2 / S3) — uses
  hardware-accelerated AES-GCM via ESP-IDF
* **M-Bus interface:** any UART-attached M-Bus slave board
  (e.g. [MIKROE M-Bus Slave Click](https://www.mikroe.com/m-bus-slave-click))
* **Cabling:** RJ11 to the meter's customer interface

ESP8266 is also supported (BearSSL crypto path retained from upstream),
but untested in this fork.

## Exposed measurements

* Voltage L1, L2, L3
* Current L1, L2, L3
* Active power (+/–)
* Active energy (+/–)
* Reactive energy (+/–)
* Power factor *(EVN-special)*
* Meter number *(EVN-special)*
* Meter timestamp

## Installation

Add the component as an `external_components` source in your ESPHome YAML:

```yaml
external_components:
  - source: github://Nirostar/esphome-dlms-meter-evn
    components: [espdm]
    refresh: 1d
```

Or pin a specific ref for reproducible builds:

```yaml
external_components:
  - source: github://Nirostar/esphome-dlms-meter-evn@v1.0.0
    components: [espdm]
```

For local development, clone this repo next to your YAML and use the
local source:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [espdm]
```

## Minimal configuration

See [`example.yaml`](example.yaml) for a complete example with all
sensors. Skeleton:

```yaml
uart:
  id: mbus
  tx_pin: GPIO16
  rx_pin: GPIO17
  baud_rate: 2400
  rx_buffer_size: 2048

espdm:
  uart_id: mbus
  key: !secret dlms_decryption_key   # 32 hex chars / 16 bytes
  voltage:        { l1: meter01_voltage_l1, l2: meter01_voltage_l2, l3: meter01_voltage_l3 }
  current:        { l1: meter01_current_l1, l2: meter01_current_l2, l3: meter01_current_l3 }
  active_power:   { plus: meter01_active_power_plus, minus: meter01_active_power_minus, power_factor: meter01_power_factor }
  active_energy:  { plus: meter01_active_energy_plus, minus: meter01_active_energy_minus }
  reactive_energy:{ plus: meter01_reactive_energy_plus, minus: meter01_reactive_energy_minus }
  timestamp:   meter01_timestamp
  meternumber: meter01_meternumber
```

All listed sensors must exist as `platform: template` sensors in your
YAML — the espdm component publishes values to them by id. See
[`example.yaml`](example.yaml) for the full set.

### Optional JSON-over-MQTT bulk publish

If you also want all readings published as a single JSON message
(useful for InfluxDB or other time-series sinks), enable the optional
`mqtt:` sub-block:

```yaml
espdm:
  # ... sensors as above ...
  mqtt:
    mqtt_id: mqtt_broker   # id of your ESPHome `mqtt:` component
    topic: "meter01/data"
```

## Getting your decryption key

EVN / Netz Niederösterreich provides the 16-byte AES key on request via
their self-service portal. You'll receive a key as 32 hex characters
(e.g. `DEADBEEFCAFEBABEDEADBEEFCAFEBABE` — this is just a placeholder,
not a real key). Keep yours in `secrets.yaml`, never commit it to a
public repo.

## Migrating from the old `custom_component` setup

Replace the entire `includes:` + `custom_component:` block with the new
declarative `espdm:` section as shown above. Your existing
`platform: template` sensor definitions stay as-is — only the IDs need
to match what you reference under `espdm:`.

Remove:

```yaml
esphome:
  includes:
    - ./esphome-dlms-meter-evn   # ← delete

custom_component:                # ← delete entire block
  - lambda: |-
      auto dlms_meter = new esphome::espdm::DlmsMeter(id(mbus));
      ...
```

Add:

```yaml
external_components:
  - source: github://Nirostar/esphome-dlms-meter-evn
    components: [espdm]

espdm:
  # ... declarative config as shown above ...
```

## Hardware wiring

Unchanged from upstream:

| ESP32 pin | M-Bus board | RJ11 to meter | Notes |
|---|---|---|---|
| 3V3   | 3V3   | —      | Power |
| GND   | GND   | —      | Ground |
| GPIO-TX (e.g. 16) | RX | — | ESP TX → M-Bus RX |
| GPIO-RX (e.g. 17) | TX | — | M-Bus TX → ESP RX |
| —     | MBUS1 | pin 3  | M-Bus to meter, polarity irrelevant |
| —     | MBUS2 | pin 4  | M-Bus to meter, polarity irrelevant |

## Acknowledgements

* [DomiStyle/esphome-dlms-meter](https://github.com/DomiStyle/esphome-dlms-meter) — original implementation
* [webeandr/esphome-dlms-meter](https://github.com/webeandr/esphome-dlms-meter) — EVN-special fork (power factor + meter number)
* Espressif — for the `esp_aes_gcm_*` hardware-accelerated GCM port

## License

MIT — see [LICENSE](LICENSE).
