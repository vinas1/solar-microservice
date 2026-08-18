# ESP32-S3 Renogy Dual Controller Monitor

## Purpose

The ESP32-S3 scans for two Renogy controllers, connects to each over BLE, reads Modbus telemetry, sends the measurements to the local solar service, and detects the Rover 40 condition where PV voltage collapses to approximately battery voltage.

For the Rover 40 recovery test, the ESP32 replays the exact two-write sequence observed when the Renogy app reapplied **User battery mode**.

## Controller lookup

| Controller | Logical key | Observed name | Primary MAC | Alternate MACs | Normal observed PV | Stuck observed PV |
|---|---|---|---|---|---:|---:|
| Rover 40 | `rover_40` | `BT-TH-E72E9AF5` | `7c:72:e7:2e:9a:f5` | `80:6f:e7:2e:9a:f5` | Approximately 32 V after manually reapplying User mode | 14.0-14.1 V |
| Rover 60 | `rover_60` | `BT-TH-7D7CDD6A` | `2c:6b:7d:7c:dd:6a` | `7c:72:7d:7c:dd:6a`, `80:6f:7d:7c:dd:6a` | 28.9-32.3 V | Not observed |

## BLE lookup

| Purpose | Service UUID | Characteristic UUID | Direction |
|---|---|---|---|
| Send Modbus frames | `0000ffd0-0000-1000-8000-00805f9b34fb` | `0000ffd1-0000-1000-8000-00805f9b34fb` | ESP32/app to Renogy bridge |
| Receive Modbus frames | `0000fff0-0000-1000-8000-00805f9b34fb` | `0000fff1-0000-1000-8000-00805f9b34fb` | Renogy bridge to ESP32/app |

The captured Renogy app traffic used GATT **Write Without Response** for Modbus requests and notifications for controller responses.

## Telemetry request

The monitor reads 34 registers beginning at `0x0100`:

```text
FF 03 01 00 00 22 <CRC low> <CRC high>
```

Expected response size:

```text
1 address + 1 function + 1 byte count + 68 data bytes + 2 CRC = 73 bytes
```

## Parsed telemetry lookup

Offsets are byte offsets in the complete Modbus response, including the three-byte response header.

| Measurement | Response offset | Scale | Example Rover 40 | Example Rover 60 |
|---|---:|---:|---:|---:|
| Battery SOC | 3 | `1` | 47-85% observed | 53-94% observed |
| Battery voltage | 5 | `0.1 V` | 12.1-13.3 V observed | 12.2-13.3 V observed |
| Charging current | 7 | `0.01 A` | 2.3-11.9 A observed | 7.0-29.5 A observed |
| PV voltage | 17 | `0.1 V` | 14.0-14.1 V while stuck | 28.9-32.3 V observed |
| PV current | 19 | `0.01 A` | 2.1-11.2 A observed | 2.8-13.7 A observed |

## Rover 40 detection

A Rover 40 sample is considered stuck when:

```cpp
controllerKey == "rover_40" &&
pvVolts > 12.0f &&
pvVolts < 15.0f
```

The ESP32 requires three qualifying Rover 40 samples before attempting recovery. Rover 60 polls do not reset the Rover 40 counter.

A successful transmission starts a 30-minute cooldown. The cooldown prevents repeated configuration writes while the result is observed.

## Captured User-mode recovery sequence

The Android Bluetooth HCI capture showed that the Renogy app sent these two Modbus function `0x06` writes to the Rover 40:

```text
FF 06 E0 02 00 C8 0B 82
```

Decoded:

- Bridge address: `0xFF`
- Function: `0x06` — write one register
- Register: `0xE002`
- Value: `0x00C8` (decimal 200)
- CRC: `0x820B`, transmitted low byte first as `0B 82`

The app then waited approximately 300 ms and sent:

```text
FF 06 E0 04 00 00 EA 15
```

Decoded:

- Bridge address: `0xFF`
- Function: `0x06`
- Register: `0xE004`
- Value: `0x0000` — observed User battery mode value
- CRC: `0x15EA`, transmitted low byte first as `EA 15`

The controller echoed both frames, indicating that both writes were accepted at the Modbus interface.

## Recovery flow

1. Poll Rover 40.
2. Detect PV voltage between 12 V and 15 V for three Rover 40 samples.
3. Verify the feature flag is enabled and the cooldown has elapsed.
4. Send `0xE002 = 0x00C8` using Write Without Response.
5. Continue servicing OTA, Telnet, and Wi-Fi for approximately 300 ms.
6. Send `0xE004 = 0x0000` using Write Without Response.
7. Start the 30-minute cooldown.
8. Verify recovery from later telemetry; expected Rover 40 PV voltage is approximately 32 V under comparable solar conditions.

## Configuration lookup

| Constant | Current value | Purpose |
|---|---:|---|
| `ENABLE_BATTERY_TYPE_REAPPLY` | `true` | Kill switch for automatic recovery |
| `MPPT_REAPPLY_CONFIRMATION_POLLS` | `3` | Consecutive Rover 40 detections required |
| `MPPT_REAPPLY_COOLDOWN_MS` | `1800000` | 30-minute retry suppression |
| `USER_MODE_APPLY_REGISTER` | `0xE002` | First captured app write |
| `USER_MODE_APPLY_VALUE` | `0x00C8` | First captured app value |
| `BATTERY_TYPE_REGISTER` | `0xE004` | Captured battery-type register |
| `BATTERY_TYPE_USER` | `0x0000` | Captured User-mode value |
| `USER_MODE_WRITE_GAP_MS` | `300` | Delay observed between app writes |
| `MODBUS_DEVICE_ADDRESS` | `0xFF` | Renogy BLE bridge/controller address |

## Other captured configuration data

The app read a 33-register block beginning at `0xE001`. The capture also showed writes across the `0xE002-0xE01F` configuration area while settings were being applied.

| Register | Observed value | Notes |
|---|---:|---|
| `0xE002` | `0x00C8` | Rewritten immediately before User mode selection |
| `0xE003` | `0x0C00` | Observed app write; exact meaning not confirmed |
| `0xE004` | `0x0000` or `0x0001` | `0x0000` was captured when User mode was reapplied |
| `0xE00B` | `0x007E` | Observed configuration value; meaning not confirmed |
| `0xE00C` | `0x0078` | Observed configuration value; meaning not confirmed |
| `0xE00D` | `0x006F` | Observed configuration value; meaning not confirmed |
| `0xE00E` | `0x006A` | Observed configuration value; meaning not confirmed |
| `0xE010` | `0x0005` | Observed configuration value; meaning not confirmed |
| `0xE011` | `0x0078` | Observed configuration value; meaning not confirmed |
| `0xE012` | `0x0078` | Observed configuration value; meaning not confirmed |
| `0xE013` | `0x001E` | Observed configuration value; meaning not confirmed |
| `0xE014` | `0x0003` | Observed configuration value; meaning not confirmed |
| `0xE01D` | `0x000F` | Observed app write; likely related to load mode because the controller displayed mode 15, but not formally verified |

Do not automate the other configuration writes unless their purpose is confirmed. Several writes returned Modbus exception `0x86 0x03`, meaning the controller rejected the supplied value.

## Rover 60 observations

The Rover 60 used the same Renogy BLE services and Modbus framing. The capture showed the app reading the same configuration block beginning at `0xE001`, but no Rover 60 MPPT fault or recovery requirement was observed.

Keep all automatic recovery logic restricted to `controllerKey == "rover_40"`.

## Validation and logging

Expected recovery logs:

```text
[MPPT] Rover 40 stuck near battery voltage. Replaying captured Renogy User-mode sequence...
[MPPT] TX: FF 06 E0 02 00 C8 0B 82
[MPPT] TX: FF 06 E0 04 00 00 EA 15
[MPPT] Captured User-mode sequence transmitted. Watching Rover 40 PV voltage for recovery.
```

Success is confirmed by subsequent Rover 40 telemetry moving from approximately 14 V toward the normal array MPPT range, not merely by successful BLE transmission.

## Operational notes

- OTA and Telnet remain serviced during response waits and the 300 ms write gap.
- BLE scans are asynchronous with a 160 ms interval and 120 ms window.
- Modbus telemetry responses are header-checked, length-checked, and CRC-checked.
- Shared notification data is copied under an ESP32 critical section before parsing.
- HTTP telemetry is sent once per successful controller poll.
- The recovery cooldown is stored only in RAM and resets after reboot or OTA update.
