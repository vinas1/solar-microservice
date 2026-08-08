#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <ArduinoOTA.h>

// written by: @vinas1 visit me on GitHub: vinas1.github.io or see the original project at: github.com/vinas1/esp32-bluetooth-collector

// ============================================================================
// CONFIGURATION
// ============================================================================

static constexpr const char* WIFI_SSID         = "wifi_ssid";
static constexpr const char* WIFI_PASS         = "wifi_password";
static constexpr const char* OTA_PASSWORD      = "ota_password";
static constexpr const char* SOLAR_SERVICE_URL = "http://192.168.0.60:30500/api/renogy";

static constexpr uint32_t WIFI_RETRY_MS        = 5000;
static constexpr uint32_t POLL_INTERVAL_MS     = 15000;
static constexpr uint32_t SCAN_DURATION_MS     = 5000;
static constexpr uint32_t CONNECT_TIMEOUT_MS   = 5000;
static constexpr uint32_t RESPONSE_TIMEOUT_MS  = 3000;

static constexpr uint8_t  MODBUS_DEVICE_ADDRESS = 0xFF;
static constexpr uint16_t MODBUS_START_REGISTER = 0x0100;
static constexpr uint16_t MODBUS_REGISTER_COUNT = 34;

static constexpr size_t MODBUS_REQUEST_SIZE  = 8;
static constexpr size_t MODBUS_RESPONSE_SIZE = 73;
static constexpr size_t RX_BUFFER_SIZE       = 128;

// ============================================================================
// RENOGY CONTROLLER MAC ADDRESSES - These are used to identify the specific controllers in the network. 
// Update these values with the actual MAC addresses of your Renogy controllers for accurate identification and data collection.
// ============================================================================
static constexpr const char* ROVER_40_MAC_A = "7c:72:e7:2e:9a:f5";
static constexpr const char* ROVER_40_MAC_B = "80:6f:e7:2e:9a:f5";
static constexpr const char* ROVER_60_MAC_A = "2c:6b:7d:7c:dd:6a";
static constexpr const char* ROVER_60_MAC_B = "7c:72:7d:7c:dd:6a";
static constexpr const char* ROVER_60_MAC_C = "80:6f:7d:7c:dd:6a";

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

// ============================================================================
// You can verify the output by connecting to the ESP32 via telnet on port 23. 
// Use a telnet client to connect to the ESP32's IP address and port 23 to view the logs and debug information.
// This can be useful for monitoring the system's behavior and troubleshooting any issues that may arise during operation.
// example: nc 192.168.0.224 23
// ============================================================================
WiFiServer telnetServer(23);
WiFiClient telnetClient;

static uint8_t rxBuffer[RX_BUFFER_SIZE];
static volatile size_t rxLength = 0;
static volatile bool rxComplete = false;
static volatile bool rxOverflow = false;

// ============================================================================
// SYSTEM SERVICES
// ============================================================================

void remoteLog(const String& message) {
    Serial.println(message);

    if (telnetClient && telnetClient.connected()) {
        telnetClient.println(message);
    }
}

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
    http.begin(SOLAR_SERVICE_URL);
    http.addHeader("Content-Type", "application/json");

    String jsonPayload = "{";
    jsonPayload += "\"" + controllerKey + "\":{";
    jsonPayload += "\"battery_soc\":" + String(soc) + ",";
    jsonPayload += "\"battery_volts\":" + String(bVolts, 1) + ",";
    jsonPayload += "\"charging_amps\":" + String(cAmps, 2) + ",";
    jsonPayload += "\"pv_volts\":" + String(pVolts, 1) + ",";
    jsonPayload += "\"pv_amps\":" + String(pAmps, 2);
    jsonPayload += "}}";

    int httpCode = http.POST(jsonPayload);

    if (httpCode > 0) {
        remoteLog("[HTTP] POST " + String(httpCode) + " -> solar-service (" + controllerKey + ")");
    } else {
        remoteLog("[HTTP] POST failed: " + http.errorToString(httpCode));
    }

    http.end();
}

// ============================================================================
// BLE CLIENT CLEANUP
// ============================================================================

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

void resetResponseBuffer() {
    rxLength = 0;
    rxComplete = false;
    rxOverflow = false;
    memset(rxBuffer, 0, sizeof(rxBuffer));
}

void notifyCallback(
    NimBLERemoteCharacteristic* characteristic,
    uint8_t* data,
    size_t length,
    bool isNotify
) {
    (void)characteristic;
    (void)isNotify;

    if (!data || length == 0 || rxComplete) {
        return;
    }

    const size_t currentLength = rxLength;

    if (currentLength >= sizeof(rxBuffer)) {
        rxOverflow = true;
        return;
    }

    const size_t available =
        sizeof(rxBuffer) - currentLength;

    const size_t copyLength =
        length < available ? length : available;

    memcpy(
        rxBuffer + currentLength,
        data,
        copyLength
    );

    rxLength = currentLength + copyLength;

    if (copyLength < length) {
        rxOverflow = true;
        return;
    }

    if (rxLength >= 3) {
        const size_t expectedLength =
            static_cast<size_t>(rxBuffer[2]) + 5;

        if (
            expectedLength <= sizeof(rxBuffer) &&
            rxLength >= expectedLength
        ) {
            rxComplete = true;
        }
    }
}

// ============================================================================
// BLE HELPERS
// ============================================================================

bool isTargetController(
    const String& name,
    const String& mac
) {
    return (
        name.startsWith("BT-TH-") ||
        mac.equalsIgnoreCase(ROVER_40_MAC_A) ||
        mac.equalsIgnoreCase(ROVER_40_MAC_B) ||
        mac.equalsIgnoreCase(ROVER_60_MAC_A) ||
        mac.equalsIgnoreCase(ROVER_60_MAC_B) ||
        mac.equalsIgnoreCase(ROVER_60_MAC_C)
    );
}

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
        static_cast<uint32_t>(
            millis() - responseStart
        ) < RESPONSE_TIMEOUT_MS
    ) {
        serviceSystem();
        delay(10);
    }

    notifyCharacteristic->unsubscribe();

    const size_t finalLength = rxLength;

    if (rxOverflow) {
        remoteLog("[MODBUS] Error: Response buffer overflow.");
        cleanupClient(client);
        return;
    }

    if (!rxComplete) {
        remoteLog(
            "[MODBUS] Error: Notification timeout. Received " +
            String(finalLength) +
            " bytes."
        );

        cleanupClient(client);
        return;
    }

    if (!validateModbusResponse(rxBuffer, finalLength)) {
        cleanupClient(client);
        return;
    }

    const uint16_t batterySoc =
        readRegister(rxBuffer, 3);

    const float batteryVolts =
        readRegister(rxBuffer, 5) * 0.1f;

    const float chargingAmps =
        readRegister(rxBuffer, 7) * 0.01f;

    const float pvVolts =
        readRegister(rxBuffer, 17) * 0.1f;

    const float pvAmps =
        readRegister(rxBuffer, 19) * 0.01f;

    const String controllerKey = getControllerKey(deviceMac);

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

    sendToSolarService(controllerKey, batterySoc, batteryVolts, chargingAmps, pvVolts, pvAmps);

    cleanupClient(client);
    remoteLog("[BLE] Disconnected cleanly.");
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(250);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
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
// MAIN LOOP
// ============================================================================

void loop() {
    serviceSystem();

    static uint32_t lastPoll = 0;
    const uint32_t now = millis();

    if (
        lastPoll != 0 &&
        static_cast<uint32_t>(
            now - lastPoll
        ) < POLL_INTERVAL_MS
    ) {
        delay(5);
        return;
    }

    lastPoll = now;

    remoteLog("[BLE] Scanning for Renogy controllers...");

    NimBLEScan* scanner =
        NimBLEDevice::getScan();

    if (!scanner) {
        remoteLog("[BLE] Error: Scanner unavailable.");
        return;
    }

    scanner->setActiveScan(true);
    scanner->setInterval(160);
    scanner->setWindow(160);
    scanner->setMaxResults(30);

    NimBLEScanResults results =
        scanner->getResults(
            SCAN_DURATION_MS,
            false
        );

    serviceSystem();

    for (
        int index = 0;
        index < results.getCount();
        index++
    ) {
        const NimBLEAdvertisedDevice* device =
            results.getDevice(index);

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
// written by: @vinas1 visit me on GitHub: vinas1.github.io or see the original project at: github.com/vinas1/esp32-bluetooth-collector