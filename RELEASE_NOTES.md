# Release Notes — v1.0.0

## Initial Release

This is the initial stable release of **solar-microservice**, an end-to-end telemetry pipeline for Renogy Rover solar equipment over Bluetooth Low Energy (BLE).

### What's included

- **ESP32-S3 BLE probe firmware** — reads Modbus RTU data from Renogy Rover charge controllers via BLE GATT
- **Python FastAPI microservice** — receives HTTP POST payloads and forwards to Zabbix trapper
- **Docker container** — packaged for deployment on k3s / Kubernetes clusters
- **Zabbix dashboards** — real-time visualization and historical graphing of solar telemetry data
- **GitHub Container Registry (GHCR)** images for the microservice

### Supported hardware

- Renogy Rover 40 MPPT
- Renogy Rover 60 MPPT
- Renogy BT-1 and BT-2 adapters
- ESP32-S3 DevKitC-1 N16R8 (recommended)

### Architecture overview

```
Renogy Controllers → BLE GATT → ESP32-S3 Probe → HTTP POST → FastAPI Microservice → Zabbix Server
```

### Repository structure

| Path | Description |
|------|-------------|
| `src/esp32-ble-probe.cpp` | ESP32-S3 firmware source |
| `microservice/app.py` | FastAPI microservice |
| `microservice/Dockerfile` | Container definition |
| `zabbix/*.json` | Zabbix dashboard templates |
| `include/`, `lib/` | PlatformIO libraries and headers |

### Requirements

- Git, Arduino IDE or PlatformIO, ESP32 Arduino core, NimBLE-Arduino 2.x
- Python, Docker, kubectl, Kubernetes/k3s cluster, Zabbix server

---

# Release Notes — v1.1.0

## Minor Release Update

### What's included
- General improvements and bug fixes.
- Performance optimizations for the FastAPI microservice.
- Updated documentation for better clarity.