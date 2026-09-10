# Architecture Documentation

This directory contains all architectural diagrams and system visualizations generated via the `archify` tool.

## Overview

The solar microservice architecture consists of several key components:
- **ESP32/Rover Hardware**: Edge devices collecting sensor data.
- **Modbus Communication**: Protocol used for Renogy device interaction.
- **Zabbix Monitoring**: System for health and performance tracking.
- **Python Microservice**: The core logic processing data and managing integrations.

## Diagrams

### System Runtime Architecture
- **Interactive Visual Artifact:** [`solar-microservice-runtime.architecture.html`](./solar-microservice-runtime.architecture.html)
- **Structured Source Specification:** [`solar-microservice-runtime.architecture.json`](./solar-microservice-runtime.architecture.json)
- **Visual Validation Report:** [`solar-microservice-runtime.architecture.visual-check.html`](./solar-microservice-runtime.architecture.visual-check.html)

The diagram models the complete off-grid telemetry loop across 4 boundaries:
1. **Off-Grid Solar Site (Physical & BLE):** Renogy Rover 40 & 60 MPPT controllers and BT-1/BT-2 adapters.
2. **ESP32-S3 Edge Node (WiFi STA):** NimBLE telemetry engine, cooperative ArduinoOTA updates, and Telnet live console.
3. **K3s Cluster (Node 192.168.0.60):** Kubernetes NodePort service (port 30500) and FastAPI microservice piping metrics to `zabbix_sender`.
4. **Observability Host (gsdebian):** Zabbix Server (port 10051) and real-time solar dashboards.

