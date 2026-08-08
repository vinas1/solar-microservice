**Macro Objective & System Goal**

- **Purpose:** Automated, off-grid solar power system telemetry monitoring. The probe tracks two Renogy Rover MPPT charge controllers (Rover 40 and Rover 60) managing deep-cycle battery banks at a remote installation.
    
- **End-to-End Flow:** `ESP32-S3 Probe` $\rightarrow$ `K3s FastAPI Ingestion Service` $\rightarrow$ `Zabbix Trapper` $\rightarrow$ `Zabbix Dashboard & Alerting System`.
    

**Toolchain & Build Environment (`platformio.ini`)**

- **Platform/Framework:** `espressif32`, Arduino framework targeted for ESP32-S3 (e.g., `esp32-s3-devkitc-1`).
    
- **Required Libraries:**
    
    - `h2zero/NimBLE-Arduino` (v2.5.0 or higher)
        
    - `bblanchon/ArduinoJson` (if refactoring string construction)
        
- **Flash & Partition Table Requirements:** Must use a partition scheme with enlarged app partitions (such as `min_spiffs.csv` or `huge_app.csv`) to accommodate both the NimBLE stack and `ArduinoOTA` flash memory overhead.
    

**Downstream Pipeline Contract (FastAPI & Zabbix)**

- **Ingestion Container:** FastAPI app running inside the local K3s cluster at `[http://192.168.0.60:30500/api/renogy](http://192.168.0.60:30500/api/renogy)`.
    
- **Processing:** The API parses the incoming payload (`rover_40` or `rover_60` top-level keys) and reformats the metrics into Zabbix Trapper items using `zabbix-sender` or native TCP socket payloads to the Zabbix server.
    
- **Expected Zabbix Item Keys:**
    
    - `renogy[rover_40,battery_soc]`
        
    - `renogy[rover_60,battery_volts]`
        
    - `renogy[rover_60,pv_amps]`
        

**Physical Environment & Edge Operational Behavior**

- **Radio Constraints:** BLE range attenuation between the probe and Renogy modules must be under 15 feet.
    
- **Nighttime / Solar Sleep Behavior:** During low/zero PV input (nighttime), Renogy BLE modules occasionally drop connections or delay responses. The firmware's non-blocking timeouts (`CONNECT_TIMEOUT_MS = 5000`, `RESPONSE_TIMEOUT_MS = 3000`) ensure a unresponsive controller never hangs the probe or blocks the remaining controller from polling.
    
- **Recovery Protocol:** If Wi-Fi connection drops, the probe continues loop execution locally, attempting background reconnection every 5 seconds without resetting BLE stack state or corrupting buffers.



**System Identity & Hardware Target**

* **Device Platform:** ESP32-S3 microcontroller running Arduino framework with NimBLE-Arduino (v2.5.1+).
* **Firmware Core:** `ESP32S3-Renogy` edge probe executing a non-blocking 15-second polling loop (`POLL_INTERVAL_MS = 15000`).
* **Cooperative Multitasking:** `serviceSystem()` yields CPU execution to process background Wi-Fi reconnects, handle `ArduinoOTA.handle()`, and maintain non-blocking socket connections on the Telnet logging server.

**Network Credentials & Management Interfaces**

* **Wi-Fi Infrastructure:** SSID `YOUR_SSID` | WPA2 Key `WIFI_PW` | Auto-reconnect interval `5000ms`.
* **OTA Firmware Flashing:** Hostname `esp32-ble-probe` | Auth Password `OTA_PW` | Port 3232.
* **Console Mirroring:** Telnet server on TCP port 23 (`remoteLog()` broadcasts to both `Serial` and active Telnet client sockets).

**BLE Scanning & GATT Profile Specification**

* **Scan Parameters:** Active scanning enabled (`INTERVAL = 160`, `WINDOW = 160`, `DURATION = 5000ms`, `MAX_RESULTS = 30`). Target filtering matches device names starting with `BT-TH-` or explicit MAC matches.
* **GATT Characteristic Architecture:**
* **TX Command Pipe:** Service `0000ffd0-0000-1000-8000-00805f9b34fb` | Characteristic `0000ffd1-0000-1000-8000-00805f9b34fb` (Write with/without response).
* **RX Notification Pipe:** Service `0000fff0-0000-1000-8000-00805f9b34fb` | Characteristic `0000fff1-0000-1000-8000-00805f9b34fb` (Notify).


* **Subscription Order Constraint:** Firmware must subscribe to notifications on `FFF1` *prior* to issuing the Modbus write frame on `FFD1`. Reading directly from `FFD1` or failing to subscribe to `FFF1` returns empty payloads or unreadable characteristic errors.

**Modbus RTU Protocol & Frame Construction**

* **Read Request Frame (8 Bytes):** `[0xFF, 0x03, 0x01, 0x00, 0x00, 0x22, 0xCE, 0xA0]`
* `0xFF`: Broadcast/Device Address
* `0x03`: Function Code (Read Holding Registers)
* `0x0100`: Start Register Address (Register 256)
* `0x0022`: Register Count (34 registers / 68 data bytes)
* `0xCEA0`: Modbus CRC-16 (Polynomial `0xA001`, initial value `0xFFFF`, byte-swapped low/high).


* **Response Packet Validation:**
* **Expected Byte Length:** 73 bytes (`1 byte Addr + 1 byte FC + 1 byte ByteCount [0x44] + 68 bytes Data + 2 bytes CRC`).
* **CRC Checksum:** Standard Modbus CRC-16 calculated over the first 71 bytes and verified against trailing 2 bytes.



**Binary Telemetry Parsing Map**

| Metric | Buffer Byte Offset | Data Type | Register Scale Factor | Target Output Unit |
| --- | --- | --- | --- | --- |
| **Battery SOC** | `rxBuffer[3..4]` | `uint16_t` (Big-Endian) | $1:1$ | % |
| **Battery Volts** | `rxBuffer[5..6]` | `uint16_t` (Big-Endian) | $0.1$ | Volts (V) |
| **Charging Amps** | `rxBuffer[7..8]` | `uint16_t` (Big-Endian) | $0.01$ | Amperes (A) |
| **PV Input Volts** | `rxBuffer[17..18]` | `uint16_t` (Big-Endian) | $0.1$ | Volts (V) |
| **PV Input Amps** | `rxBuffer[19..20]` | `uint16_t` (Big-Endian) | $0.01$ | Amperes (A) |

**Ingestion API Mapping & Payload Schema**

* **HTTP Endpoint:** `POST [http://192.168.0.60:30500/api/renogy](http://192.168.0.60:30500/api/renogy)` (`Content-Type: application/json`).
* **MAC Address Resolution:**
* `7c:72:e7:2e:9a:f5` or `80:6f:e7:2e:9a:f5` $\rightarrow$ `"rover_40"`
* `2c:6b:7d:7c:dd:6a`, `7c:72:7d:7c:dd:6a`, or `80:6f:7d:7c:dd:6a` $\rightarrow$ `"rover_60"`


* **JSON Payload Format:**
```json
{
  "rover_60": {
    "battery_soc": 100,
    "battery_volts": 13.5,
    "charging_amps": 12.60,
    "pv_volts": 31.5,
    "pv_amps": 5.61
  }
}

```



**Memory & Resource Safeguards**

* **Static Allocation:** 128-byte static buffer (`rxBuffer`) prevents heap fragmentation during continuous string/byte ops.
* **Heap Leak Prevention:** All client connections pass through `cleanupClient(NimBLEClient*&)` which explicitly enforces `client->disconnect()` followed by `NimBLEDevice::deleteClient(client)` to clear BLE stack memory on every connection attempt.
* **Execution Guardrails:** Connection timeout set to $5000\text{ ms}$; response timeout guarded at $3000\text{ ms}$ via non-blocking `millis()` subtraction.





## K3s FastAPI Ingestion Microservice Specification

This section details the runtime environment, application architecture, API contract, and Zabbix trapper forwarding logic for the backend container running in the K3s cluster.

### 1. Deployment & Runtime Environment

- **Container Stack:** Python 3.11+, FastAPI ASGI framework, Uvicorn production server, and `pyzabbix` / `zabbix-utils` sender client.
    
- **K3s Service Configuration:**
    
    - **Deployment Type:** Kubernetes Deployment (`1` to `2` replicas).
        
    - **Service Type:** `NodePort` exposed on port `30500`.
        
    - **External Endpoint:** `[http://192.168.0.60:30500/api/renogy](http://192.168.0.60:30500/api/renogy)`
        
    - **Liveness / Readiness Probes:** `GET /healthz` returning `{"status": "ok"}` on port `8000` internally.
        

### 2. API Contract & Data Processing Pipeline

#### Endpoint: `POST /api/renogy`

- **Headers:** `Content-Type: application/json`
    
- **Ingestion Workflow:**
    
    1. Receives and validates incoming JSON payload from the ESP32-S3 probe.
        
    2. Extracts top-level key (`rover_40` or `rover_60`).
        
    3. Iterates through sub-keys (`battery_soc`, `battery_volts`, `charging_amps`, `pv_volts`, `pv_amps`).
        
    4. Maps metrics into Zabbix Trapper format.
        
    5. Asynchronously sends batch payloads to the Zabbix Server/Proxy trapper port (`10051`).
        

#### Expected Input Payload Schema

JSON

```
{
  "rover_60": {
    "battery_soc": 100,
    "battery_volts": 13.5,
    "charging_amps": 12.60,
    "pv_volts": 31.5,
    "pv_amps": 5.61
  }
}
```

#### FastAPI Pydantic Validation Models

Python

```
from pydantic import BaseModel, Field
from typing import Dict, Optional

class ControllerTelemetry(BaseModel):
    battery_soc: int = Field(..., ge=0, le=100)
    battery_volts: float = Field(..., ge=0.0)
    charging_amps: float = Field(..., ge=0.0)
    pv_volts: float = Field(..., ge=0.0)
    pv_amps: float = Field(..., ge=0.0)

class RenogyPayload(BaseModel):
    rover_40: Optional[ControllerTelemetry] = None
    rover_60: Optional[ControllerTelemetry] = None
```

### 3. Zabbix Trapper Mapping Specification

- **Zabbix Server / Proxy Host:** `192.168.0.x` (or cluster-internal Zabbix endpoint) on Port `10051`.
    
- **Zabbix Monitored Host Name:** `Offgrid-Solar-System` (or per-controller host identifiers `rover_40` / `rover_60`).
    
- **Item Key Structure:** `renogy[<controller_id>,<metric_name>]`
    

|**Incoming Json Metric**|**Target Zabbix Trapper Key**|**Data Type**|
|---|---|---|
|`battery_soc`|`renogy[rover_40,battery_soc]` or `renogy[rover_60,battery_soc]`|Numeric (Unsigned)|
|`battery_volts`|`renogy[rover_40,battery_volts]` or `renogy[rover_60,battery_volts]`|Numeric (Float)|
|`charging_amps`|`renogy[rover_40,charging_amps]` or `renogy[rover_60,charging_amps]`|Numeric (Float)|
|`pv_volts`|`renogy[rover_40,pv_volts]` or `renogy[rover_60,pv_volts]`|Numeric (Float)|
|`pv_amps`|`renogy[rover_40,pv_amps]` or `renogy[rover_60,pv_amps]`|Numeric (Float)|

### 4. Microservice Error Handling & Logging

- **Validation Failures:** Returns `HTTP 422 Unprocessable Entity` if payload schema violates Pydantic types; prevents bad telemetry from polluting Zabbix history.
    
- **Zabbix Unreachable:** If the Zabbix trapper socket times out, the microservice logs a warning via Python `logging`, returns `HTTP 502 Bad Gateway` or `HTTP 500` to the ESP32 probe, but remains alive.
    
- **Idempotency:** Accepts independent POST requests for individual controllers (`rover_40` only or `rover_60` only) without requiring both controllers to be present in a single HTTP request.
