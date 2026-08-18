#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <ArduinoOTA.h>

// ============================================================================
// CONFIGURATION
// Timing values are milliseconds. MAC aliases account for addresses observed
// from the same Renogy controllers across different BLE adapter revisions.
// ============================================================================

// masked creds
static constexpr const char* WIFI_SSID         = "wifi";
static constexpr const char* WIFI_PASS         = "wifipass";
static constexpr const char* OTA_PASSWORD      = "otapass";
static constexpr const char* SOLAR_SERVICE_URL = "http://192.168.0.60:30500/api/renogy";

static constexpr uint32_t WIFI_RETRY_MS        = 5000;
static constexpr uint32_t POLL_INTERVAL_MS     = 15000;
static constexpr uint32_t SCAN_DURATION_MS     = 5000;
static constexpr uint32_t CONNECT_TIMEOUT_MS   = 5000;
static constexpr uint32_t RESPONSE_TIMEOUT_MS  = 3000;
static constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 2000;
static constexpr uint32_t HTTP_RESPONSE_TIMEOUT_MS = 3000;
static constexpr uint32_t WIFI_STARTUP_TIMEOUT_MS = 15000;
static constexpr bool     ENABLE_BATTERY_TYPE_REAPPLY = true;
static constexpr uint32_t MPPT_REAPPLY_COOLDOWN_MS = 1800000; // 30 minutes
static constexpr uint8_t  MPPT_REAPPLY_CONFIRMATION_POLLS = 3;

static constexpr uint8_t  MODBUS_DEVICE_ADDRESS = 0xFF;
static constexpr uint16_t MODBUS_START_REGISTER = 0x0100;
static constexpr uint16_t MODBUS_REGISTER_COUNT = 34;

static constexpr size_t MODBUS_REQUEST_SIZE  = 8;
static constexpr size_t MODBUS_RESPONSE_SIZE = 73;
static constexpr size_t RX_BUFFER_SIZE       = 128;

static constexpr const char* ROVER_40_MAC_A = "7c:72:e7:2e:9a:f5";
static constexpr const char* ROVER_40_MAC_B = "80:6f:e7:2e:9a:f5";
static constexpr const char* ROVER_60_MAC_A = "2c:6b:7d:7c:dd:6a";
static constexpr const char* ROVER_60_MAC_B = "7c:72:7d:7c:dd:6a";
static constexpr const char* ROVER_60_MAC_C = "80:6f:7d:7c:dd:6a";

// Captured from the Renogy app while User battery mode was reapplied to this
// Rover 40. Keep both writes together and preserve the observed 300 ms gap.
static constexpr uint16_t USER_MODE_APPLY_REGISTER = 0xE002;
static constexpr uint16_t USER_MODE_APPLY_VALUE    = 0x00C8;
static constexpr uint16_t BATTERY_TYPE_REGISTER    = 0xE004;
static constexpr uint16_t BATTERY_TYPE_USER        = 0x0000;
static constexpr uint32_t USER_MODE_WRITE_GAP_MS   = 300;

static const NimBLEUUID TX_SERVICE_UUID(
    "0000ffd0-0000-1000-8000-00805f9b34fb"
);

static const NimBLEUUID TX_CHAR_UUID(
    "0000ffd1-0000-1000-8000-00805f9b34fb"
);

static const NimBLEUUID RX_SERVICE_UUID(
    "0000fff0-0000-1000-8000-00805f9b34fb"
);

static const NimBLEUUID RX_CHAR_UUID(
    "0000fff1-0000-1000-8000-00805f9b34fb"
);

WiFiServer telnetServer(23);
WiFiClient telnetClient;

// Notification callbacks run in the NimBLE task while pollController() runs
// in the Arduino task. rxMux protects this shared Modbus response state.
static uint8_t rxBuffer[RX_BUFFER_SIZE];
static volatile size_t rxLength = 0;
static volatile bool rxComplete = false;
static volatile bool rxOverflow = false;
static volatile bool rxInvalid = false;
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;


// ============================================================================
// SYSTEM SERVICES
// ============================================================================

void remoteLog(const String& message) {
    Serial.println(message);

    if (telnetClient && telnetClient.connected()) {
        telnetClient.println(message);
    }
}

// Keep Wi-Fi recovery, OTA, and the single Telnet console responsive. Call this
// from any bounded wait loop instead of using one long blocking delay.
void serviceSystem() {
    static uint32_t lastWiFiRetry = 0;

    const uint32_t now = millis();

    if (
        WiFi.status() != WL_CONNECTED &&
        static_cast<uint32_t>(now - lastWiFiRetry) >= WIFI_RETRY_MS
    ) {
        lastWiFiRetry = now;
        WiFi.reconnect();
    }

    ArduinoOTA.handle();

    if (telnetServer.hasClient()) {
        WiFiClient incoming = telnetServer.available();

        if (!telnetClient || !telnetClient.connected()) {
            if (telnetClient) {
                telnetClient.stop();
            }

            telnetClient = incoming;
            telnetClient.println(
                "Connected to Renogy Dual Controller Monitor!"
            );
        } else {
            incoming.stop();
        }
    }

    yield();
}

// ============================================================================
// HTTP API EXPORT
// ============================================================================

String getControllerKey(const String& mac) {
    if (
        mac.equalsIgnoreCase(ROVER_40_MAC_A) ||
        mac.equalsIgnoreCase(ROVER_40_MAC_B)
    ) {
        return "rover_40";
    }

    if (
        mac.equalsIgnoreCase(ROVER_60_MAC_A) ||
        mac.equalsIgnoreCase(ROVER_60_MAC_B) ||
        mac.equalsIgnoreCase(ROVER_60_MAC_C)
    ) {
        return "rover_60";
    }

    return "";
}

// Send one controller sample to the local solar service. A fixed JSON buffer
// avoids repeated heap allocations during continuous operation.
void sendToSolarService(
    const String& controllerKey,
    uint16_t soc,
    float bVolts,
    float cAmps,
    float pVolts,
    float pAmps
) {
    if (WiFi.status() != WL_CONNECTED) {
        remoteLog("[HTTP] Error: WiFi disconnected, skipping POST.");
        return;
    }

    if (controllerKey.length() == 0) {
        remoteLog("[HTTP] Error: Unknown controller MAC, skipping POST.");
        return;
    }

    HTTPClient http;
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);

    if (!http.begin(SOLAR_SERVICE_URL)) {
        remoteLog("[HTTP] Error: Failed to initialize HTTP client.");
        return;
    }

    http.addHeader("Content-Type", "application/json");

    char jsonPayload[256];
    const int payloadLength = snprintf(
        jsonPayload,
        sizeof(jsonPayload),
        "{\"%s\":{\"battery_soc\":%u,\"battery_volts\":%.1f,"
        "\"charging_amps\":%.2f,\"pv_volts\":%.1f,\"pv_amps\":%.2f}}",
        controllerKey.c_str(),
        static_cast<unsigned int>(soc),
        static_cast<double>(bVolts),
        static_cast<double>(cAmps),
        static_cast<double>(pVolts),
        static_cast<double>(pAmps)
    );

    if (payloadLength < 0 || static_cast<size_t>(payloadLength) >= sizeof(jsonPayload)) {
        remoteLog("[HTTP] Error: JSON payload formatting failed.");
        http.end();
        return;
    }

    const int httpCode = http.POST(
        reinterpret_cast<uint8_t*>(jsonPayload),
        static_cast<size_t>(payloadLength)
    );

    if (httpCode >= 200 && httpCode < 300) {
        remoteLog("[HTTP] POST " + String(httpCode) + " -> solar-service (" + controllerKey + ")");
    } else if (httpCode > 0) {
        remoteLog("[HTTP] Server returned " + String(httpCode) + " (" + controllerKey + ")");
    } else {
        remoteLog("[HTTP] POST failed: " + http.errorToString(httpCode));
    }

    http.end();
}

// ============================================================================
// BLE CLIENT CLEANUP
// ============================================================================

// Disconnect and delete each short-lived BLE client on every exit path.
void cleanupClient(NimBLEClient*& client) {
    if (!client) {
        return;
    }

    if (client->isConnected()) {
        client->disconnect();
    }

    NimBLEDevice::deleteClient(client);
    client = nullptr;
}

// ============================================================================
// MODBUS
// ============================================================================

uint16_t calculateCRC(
    const uint8_t* buffer,
    size_t length
) {
    uint16_t crc = 0xFFFF;

    for (size_t position = 0; position < length; position++) {
        crc ^= static_cast<uint16_t>(buffer[position]);

        for (uint8_t bit = 0; bit < 8; bit++) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

// Build a Modbus function 0x03 request for the controller telemetry registers.
void buildModbusReadRequest(
    uint8_t deviceAddress,
    uint16_t startRegister,
    uint16_t registerCount,
    uint8_t* outputFrame
) {
    outputFrame[0] = deviceAddress;
    outputFrame[1] = 0x03;
    outputFrame[2] =
        static_cast<uint8_t>((startRegister >> 8) & 0xFF);
    outputFrame[3] =
        static_cast<uint8_t>(startRegister & 0xFF);
    outputFrame[4] =
        static_cast<uint8_t>((registerCount >> 8) & 0xFF);
    outputFrame[5] =
        static_cast<uint8_t>(registerCount & 0xFF);

    const uint16_t crc =
        calculateCRC(outputFrame, 6);

    outputFrame[6] =
        static_cast<uint8_t>(crc & 0xFF);
    outputFrame[7] =
        static_cast<uint8_t>((crc >> 8) & 0xFF);
}

void buildModbusWriteRequest(
    uint8_t deviceAddress,
    uint16_t registerAddress,
    uint16_t value,
    uint8_t* outputFrame
) {
    outputFrame[0] = deviceAddress;
    outputFrame[1] = 0x06; // Function Code 0x06: Write Single Register
    outputFrame[2] = static_cast<uint8_t>((registerAddress >> 8) & 0xFF);
    outputFrame[3] = static_cast<uint8_t>(registerAddress & 0xFF);
    outputFrame[4] = static_cast<uint8_t>((value >> 8) & 0xFF);
    outputFrame[5] = static_cast<uint8_t>(value & 0xFF);

    const uint16_t crc = calculateCRC(outputFrame, 6);

    outputFrame[6] = static_cast<uint8_t>(crc & 0xFF);
    outputFrame[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
}

// Replay the exact two-write sequence captured from the Renogy app.
// Returning true means both frames reached the BLE bridge; the following Rover
// 40 telemetry samples determine whether the controller actually recovered.
bool reapplyCapturedUserModeSequence(
    NimBLERemoteCharacteristic* writeCharacteristic
) {
    if (!writeCharacteristic || !writeCharacteristic->canWriteNoResponse()) {
        remoteLog("[MPPT] Error: FFD1 lacks Write Without Response.");
        return false;
    }

    uint8_t writeFrame[MODBUS_REQUEST_SIZE];
    buildModbusWriteRequest(
        MODBUS_DEVICE_ADDRESS,
        USER_MODE_APPLY_REGISTER,
        USER_MODE_APPLY_VALUE,
        writeFrame
    );

    remoteLog("[MPPT] TX: FF 06 E0 02 00 C8 0B 82");
    if (!writeCharacteristic->writeValue(writeFrame, sizeof(writeFrame), false)) {
        remoteLog("[MPPT] Error: Failed to transmit 0xE002=0x00C8.");
        return false;
    }

    const uint32_t gapStart = millis();
    while (static_cast<uint32_t>(millis() - gapStart) < USER_MODE_WRITE_GAP_MS) {
        serviceSystem();
        delay(10);
    }

    buildModbusWriteRequest(
        MODBUS_DEVICE_ADDRESS,
        BATTERY_TYPE_REGISTER,
        BATTERY_TYPE_USER,
        writeFrame
    );

    remoteLog("[MPPT] TX: FF 06 E0 04 00 00 EA 15");
    if (!writeCharacteristic->writeValue(writeFrame, sizeof(writeFrame), false)) {
        remoteLog("[MPPT] Error: Failed to transmit 0xE004=0x0000.");
        return false;
    }

    remoteLog(
        "[MPPT] Captured User-mode sequence transmitted. "
        "Watching Rover 40 PV voltage for recovery."
    );
    return true;
}

uint16_t readRegister(
    const uint8_t* data,
    size_t offset
) {
    return (
        static_cast<uint16_t>(data[offset]) << 8
    ) |
    static_cast<uint16_t>(data[offset + 1]);
}

bool validateModbusResponse(
    const uint8_t* data,
    size_t length
) {
    if (!data) {
        remoteLog("[MODBUS] Error: Response data is null.");
        return false;
    }

    if (length != MODBUS_RESPONSE_SIZE) {
        remoteLog(
            "[MODBUS] Error: Expected " +
            String(MODBUS_RESPONSE_SIZE) +
            " bytes, received " +
            String(length) +
            "."
        );

        return false;
    }

    if (
        (data[0] != MODBUS_DEVICE_ADDRESS && data[0] != 0x01) ||
        data[1] != 0x03 ||
        data[2] != MODBUS_REGISTER_COUNT * 2
    ) {
        remoteLog("[MODBUS] Error: Invalid response header.");
        return false;
    }

    const uint16_t calculatedCRC =
        calculateCRC(data, MODBUS_RESPONSE_SIZE - 2);

    const uint16_t receivedCRC =
        static_cast<uint16_t>(
            data[MODBUS_RESPONSE_SIZE - 2]
        ) |
        (
            static_cast<uint16_t>(
                data[MODBUS_RESPONSE_SIZE - 1]
            ) << 8
        );

    if (calculatedCRC != receivedCRC) {
        remoteLog(
            "[MODBUS] Error: CRC mismatch. Calculated=0x" +
            String(calculatedCRC, HEX) +
            ", received=0x" +
            String(receivedCRC, HEX)
        );

        return false;
    }

    return true;
}

// ============================================================================
// NOTIFICATION HANDLING
// ============================================================================

// Clear the telemetry accumulator before subscribing and sending a new query.
void resetResponseBuffer() {
    portENTER_CRITICAL(&rxMux);
    rxLength = 0;
    rxComplete = false;
    rxOverflow = false;
    rxInvalid = false;
    memset(rxBuffer, 0, sizeof(rxBuffer));
    portEXIT_CRITICAL(&rxMux);
}

// Assemble fragmented FFF1 notifications into one complete Modbus response.
// Keep this callback short: copy bytes, update flags, and return.
void notifyCallback(
    NimBLERemoteCharacteristic* characteristic,
    uint8_t* data,
    size_t length,
    bool isNotify
) {
    (void)characteristic;
    (void)isNotify;

    if (!data || length == 0) {
        return;
    }

    portENTER_CRITICAL(&rxMux);

    if (rxComplete || rxOverflow || rxInvalid) {
        portEXIT_CRITICAL(&rxMux);
        return;
    }

    const size_t currentLength = rxLength;
    const size_t available = sizeof(rxBuffer) - currentLength;

    if (length > available) {
        rxOverflow = true;
        portEXIT_CRITICAL(&rxMux);
        return;
    }

    memcpy(rxBuffer + currentLength, data, length);
    rxLength = currentLength + length;

    if (rxLength >= 1 &&
        rxBuffer[0] != MODBUS_DEVICE_ADDRESS &&
        rxBuffer[0] != 0x01) {
        rxInvalid = true;
    } else if (rxLength >= 2 && rxBuffer[1] != 0x03) {
        rxInvalid = true;
    } else if (rxLength >= 3) {
        const size_t expectedLength = static_cast<size_t>(rxBuffer[2]) + 5;

        if (rxBuffer[2] != MODBUS_REGISTER_COUNT * 2 ||
            expectedLength != MODBUS_RESPONSE_SIZE ||
            expectedLength > sizeof(rxBuffer)) {
            rxInvalid = true;
        } else if (rxLength == expectedLength) {
            rxComplete = true;
        } else if (rxLength > expectedLength) {
            rxInvalid = true;
        }
    }

    portEXIT_CRITICAL(&rxMux);
}

// ============================================================================
// BLE HELPERS
// ============================================================================

bool isTargetController(
    const String& name,
    const String& mac
) {
    (void)name;

    return (
        mac.equalsIgnoreCase(ROVER_40_MAC_A) ||
        mac.equalsIgnoreCase(ROVER_40_MAC_B) ||
        mac.equalsIgnoreCase(ROVER_60_MAC_A) ||
        mac.equalsIgnoreCase(ROVER_60_MAC_B) ||
        mac.equalsIgnoreCase(ROVER_60_MAC_C)
    );
}

// Resolve the Renogy bridge characteristics: FFD1 transmits requests to the
// controller and FFF1 returns controller notifications.
bool findRenogyCharacteristics(
    NimBLEClient* client,
    NimBLERemoteCharacteristic*& writeCharacteristic,
    NimBLERemoteCharacteristic*& notifyCharacteristic
) {
    writeCharacteristic = nullptr;
    notifyCharacteristic = nullptr;

    if (!client) {
        return false;
    }

    NimBLERemoteService* writeService =
        client->getService(TX_SERVICE_UUID);

    if (writeService) {
        writeCharacteristic =
            writeService->getCharacteristic(TX_CHAR_UUID);
    }

    NimBLERemoteService* notifyService =
        client->getService(RX_SERVICE_UUID);

    if (notifyService) {
        notifyCharacteristic =
            notifyService->getCharacteristic(RX_CHAR_UUID);
    }

    return (
        writeCharacteristic != nullptr &&
        notifyCharacteristic != nullptr
    );
}

// ============================================================================
// CONTROLLER POLLING
// ============================================================================

// Connect to one discovered controller, read registers 0x0100-0x0121, validate
// the response, publish telemetry, evaluate the lower-environment Rover 40
// User-mode reapply workaround, and disconnect.
void pollController(
    const NimBLEAdvertisedDevice* device
) {
    if (!device) {
        return;
    }

    const String deviceName =
        device->haveName()
            ? String(device->getName().c_str())
            : String("(unnamed)");

    const String deviceMac =
        String(device->getAddress().toString().c_str());

    remoteLog(
        "[BLE] Connecting to " +
        deviceName +
        " [" +
        deviceMac +
        "]..."
    );

    NimBLEClient* client =
        NimBLEDevice::createClient();

    if (!client) {
        remoteLog("[BLE] Error: Failed to allocate client.");
        return;
    }

    client->setConnectTimeout(CONNECT_TIMEOUT_MS);

    if (!client->connect(device)) {
        remoteLog(
            "[BLE] Error: Connection failed for " +
            deviceName
        );

        cleanupClient(client);
        return;
    }

    serviceSystem();

    NimBLERemoteCharacteristic* writeCharacteristic = nullptr;
    NimBLERemoteCharacteristic* notifyCharacteristic = nullptr;

    if (
        !findRenogyCharacteristics(
            client,
            writeCharacteristic,
            notifyCharacteristic
        )
    ) {
        remoteLog(
            "[BLE] Error: Required GATT characteristics unavailable."
        );

        cleanupClient(client);
        return;
    }

    if (
        !writeCharacteristic->canWrite() &&
        !writeCharacteristic->canWriteNoResponse()
    ) {
        remoteLog("[BLE] Error: FFD1 is not writable.");
        cleanupClient(client);
        return;
    }

    if (!notifyCharacteristic->canNotify()) {
        remoteLog("[BLE] Error: FFF1 cannot notify.");
        cleanupClient(client);
        return;
    }

    resetResponseBuffer();

    if (
        !notifyCharacteristic->subscribe(
            true,
            notifyCallback,
            true
        )
    ) {
        remoteLog("[BLE] Error: FFF1 subscription failed.");
        cleanupClient(client);
        return;
    }

    uint8_t requestFrame[MODBUS_REQUEST_SIZE];

    buildModbusReadRequest(
        MODBUS_DEVICE_ADDRESS,
        MODBUS_START_REGISTER,
        MODBUS_REGISTER_COUNT,
        requestFrame
    );

    const bool useWriteResponse =
        writeCharacteristic->canWrite();

    if (
        !writeCharacteristic->writeValue(
            requestFrame,
            sizeof(requestFrame),
            useWriteResponse
        )
    ) {
        remoteLog("[MODBUS] Error: FFD1 write failed.");
        notifyCharacteristic->unsubscribe();
        cleanupClient(client);
        return;
    }

    const uint32_t responseStart = millis();

    while (
        !rxComplete &&
        !rxOverflow &&
        !rxInvalid &&
        static_cast<uint32_t>(
            millis() - responseStart
        ) < RESPONSE_TIMEOUT_MS
    ) {
        serviceSystem();
        delay(10);
    }

    notifyCharacteristic->unsubscribe();

    uint8_t responseBuffer[MODBUS_RESPONSE_SIZE];
    size_t finalLength;
    bool finalComplete;
    bool finalOverflow;
    bool finalInvalid;

    portENTER_CRITICAL(&rxMux);

    finalLength = rxLength;
    finalComplete = rxComplete;
    finalOverflow = rxOverflow;
    finalInvalid = rxInvalid;

    if (finalLength <= sizeof(responseBuffer)) {
        memcpy(responseBuffer, rxBuffer, finalLength);
    }

    portEXIT_CRITICAL(&rxMux);

    if (finalOverflow) {
        remoteLog("[MODBUS] Error: Response buffer overflow.");
        cleanupClient(client);
        return;
    }

    if (finalInvalid) {
        remoteLog("[MODBUS] Error: Invalid notification frame.");
        cleanupClient(client);
        return;
    }

    if (!finalComplete) {
        remoteLog(
            "[MODBUS] Error: Notification timeout. Received " +
            String(finalLength) +
            " bytes."
        );
        cleanupClient(client);
        return;
    }

    if (!validateModbusResponse(responseBuffer, finalLength)) {
        cleanupClient(client);
        return;
    }

    const uint16_t batterySoc =
        readRegister(responseBuffer, 3);

    const float batteryVolts =
        readRegister(responseBuffer, 5) * 0.1f;

    const float chargingAmps =
        readRegister(responseBuffer, 7) * 0.01f;

    const float pvVolts =
        readRegister(responseBuffer, 17) * 0.1f;

    const float pvAmps =
        readRegister(responseBuffer, 19) * 0.01f;

    const String controllerKey = getControllerKey(deviceMac);

    const bool measurementsValid =
        batterySoc <= 100 &&
        batteryVolts >= 0.0f && batteryVolts <= 70.0f &&
        chargingAmps >= 0.0f && chargingAmps <= 200.0f &&
        pvVolts >= 0.0f && pvVolts <= 200.0f &&
        pvAmps >= 0.0f && pvAmps <= 200.0f;

    if (!measurementsValid) {
        remoteLog("[MODBUS] Error: Decoded measurements are outside expected ranges.");
        cleanupClient(client);
        return;
    }

    remoteLog("================================================");
    remoteLog(" Controller:     " + deviceName + " (" + controllerKey + ")");
    remoteLog(" MAC:            " + deviceMac);
    remoteLog(
        " Battery SOC:    " +
        String(batterySoc) +
        " %"
    );
    remoteLog(
        " Battery Volts:  " +
        String(batteryVolts, 1) +
        " V"
    );
    remoteLog(
        " Charging Amps:  " +
        String(chargingAmps, 2) +
        " A"
    );
    remoteLog(
        " PV Volts:       " +
        String(pvVolts, 1) +
        " V"
    );
    remoteLog(
        " PV Amps:        " +
        String(pvAmps, 2) +
        " A"
    );
    remoteLog("================================================");

    sendToSolarService(
        controllerKey,
        batterySoc,
        batteryVolts,
        chargingAmps,
        pvVolts,
        pvAmps
    );

    // Trigger after repeated Rover 40 samples show PV voltage collapsed near
    // battery voltage. Interleaved Rover 60 polls do not change this counter.
    static uint32_t lastRover40Reapply = 0;
    static uint8_t rover40ConditionCount = 0;

    const bool rover40PassThrough =
        controllerKey == "rover_40" &&
        pvVolts > 12.0f &&
        pvVolts < 15.0f;

    if (rover40PassThrough) {
        if (rover40ConditionCount < MPPT_REAPPLY_CONFIRMATION_POLLS) {
            rover40ConditionCount++;
        }

        const uint32_t now = millis();
        const bool reapplyCooldownElapsed =
            lastRover40Reapply == 0 ||
            static_cast<uint32_t>(now - lastRover40Reapply) >=
                MPPT_REAPPLY_COOLDOWN_MS;

        if (
            ENABLE_BATTERY_TYPE_REAPPLY &&
            rover40ConditionCount >= MPPT_REAPPLY_CONFIRMATION_POLLS &&
            reapplyCooldownElapsed
        ) {
            remoteLog(
                "[MPPT] Rover 40 stuck near battery voltage. "
                "Replaying captured Renogy User-mode sequence..."
            );

            if (reapplyCapturedUserModeSequence(writeCharacteristic)) {
                lastRover40Reapply = now;
                rover40ConditionCount = 0;
            } else {
                remoteLog("[MPPT] Error: Captured User-mode sequence failed.");
            }
        }
    } else if (controllerKey == "rover_40") {
        rover40ConditionCount = 0;
    }

    cleanupClient(client);
    remoteLog("[BLE] Disconnected cleanly.");
}

// ============================================================================
// SETUP
// ============================================================================

// Initialize network services first, then start OTA and the NimBLE stack.
void setup() {
    Serial.begin(115200);
    delay(250);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    const uint32_t wifiStart = millis();
    while (
        WiFi.status() != WL_CONNECTED &&
        static_cast<uint32_t>(millis() - wifiStart) < WIFI_STARTUP_TIMEOUT_MS
    ) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        remoteLog("[WIFI] Connected: " + WiFi.localIP().toString());
    } else {
        remoteLog("[WIFI] Startup timeout; background reconnect enabled.");
    }

    telnetServer.begin();
    telnetServer.setNoDelay(true);

    ArduinoOTA.setHostname("esp32-ble-probe");
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        remoteLog("[OTA] Update started...");
    });

    ArduinoOTA.onEnd([]() {
        remoteLog("[OTA] Update complete.");
    });

    ArduinoOTA.onError([](ota_error_t error) {
        remoteLog(
            "[OTA] Error [" +
            String(static_cast<int>(error)) +
            "]"
        );
    });

    ArduinoOTA.begin();

    NimBLEDevice::init("ESP32S3-Renogy");
    NimBLEDevice::setPower(3);

    remoteLog("[SYSTEM] Renogy monitor ready.");
}

// ============================================================================
// ASYNCHRONOUS BLE SCANNING
// ============================================================================

static volatile bool scanComplete = false;

// The callback only marks completion. Device processing stays in loop() so no
// connections, HTTP calls, or logging occur in the NimBLE scan callback.
class RenogyScanCallbacks : public NimBLEScanCallbacks {
    void onScanEnd(const NimBLEScanResults& scanResults, int reason) override {
        (void)scanResults;
        (void)reason;
        scanComplete = true;
    }
};

static RenogyScanCallbacks scanCallbacks;

// Process stored scan results after the asynchronous scan has completed.
void processScanResults(NimBLEScan* scanner) {
    if (!scanner) {
        return;
    }

    const NimBLEScanResults results = scanner->getResults();

    for (int index = 0; index < results.getCount(); index++) {
        const NimBLEAdvertisedDevice* device = results.getDevice(index);

        if (!device) {
            continue;
        }

        const String name =
            device->haveName()
                ? String(device->getName().c_str())
                : String("");

        const String mac =
            String(device->getAddress().toString().c_str());

        if (!isTargetController(name, mac)) {
            continue;
        }

        remoteLog(
            "[BLE] Discovered target: " +
            name +
            " [" +
            mac +
            "]"
        );

        pollController(device);
        serviceSystem();
    }

    scanner->clearResults();
}

// ============================================================================
// MAIN LOOP
// ============================================================================

// Start scans asynchronously so OTA, Telnet, and Wi-Fi servicing continue while
// BLE advertisements are collected.
void loop() {
    serviceSystem();

    static uint32_t lastScanStart = 0;
    NimBLEScan* scanner = NimBLEDevice::getScan();

    if (!scanner) {
        remoteLog("[BLE] Error: Scanner unavailable.");
        delay(1000);
        return;
    }

    if (scanComplete) {
        scanComplete = false;
        processScanResults(scanner);
    }

    const uint32_t now = millis();
    const bool scanDue =
        lastScanStart == 0 ||
        static_cast<uint32_t>(now - lastScanStart) >= POLL_INTERVAL_MS;

    if (scanDue && !scanner->isScanning()) {
        scanner->setActiveScan(true);
        scanner->setInterval(160);
        scanner->setWindow(120);
        scanner->setMaxResults(30);
        scanner->setScanCallbacks(&scanCallbacks, false);

        remoteLog("[BLE] Scanning for Renogy controllers...");

        if (scanner->start(SCAN_DURATION_MS, false, true)) {
            lastScanStart = now;
        } else {
            remoteLog("[BLE] Error: Failed to start scan.");
        }
    }

    delay(5);
}
