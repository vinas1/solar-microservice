Common issues:

### I installed a new Renogy Rover charge controller and it's not being found.


Open the Renogy App and verify that the bluetooth stack is broadcasting by pressing the + button and adding a device. 

You should see a screen like this:

<img width="523" height="1276" alt="image" src="https://github.com/user-attachments/assets/e389a98a-77e4-4203-b7c0-99ceaee2bb70" />


Restart the  ESP32S3 (Arduino Device)
telnet to the ESP32S3 (Arduino Device)

```
 Controller:     BT-TH-7D7CDD6A (rover_60)
 MAC:            2c:6b:7d:7c:dd:6a
 Battery SOC:    100 %
 Battery Volts:  13.3 V
 Charging Amps:  13.20 A
 PV Volts:       31.4 V
 PV Amps:        5.82 A
================================================
[HTTP] POST 200 -> solar-service (rover_60)
[BLE] Disconnected cleanly.
[BLE] Scanning for Renogy controllers...
[BLE] Discovered target: BT-TH-E72E9AF5 [7c:72:e7:2e:9a:f5]
[BLE] Connecting to BT-TH-E72E9AF5 [7c:72:e7:2e:9a:f5]...
================================================
 Controller:     BT-TH-E72E9AF5 (rover_40)
 MAC:            7c:72:e7:2e:9a:f5
 Battery SOC:    72 %
 Battery Volts:  13.1 V
 Charging Amps:  0.00 A
 PV Volts:       40.6 V
 PV Amps:        0.00 A
================================================
[HTTP] POST 200 -> solar-service (rover_40)
[BLE] Disconnected cleanly.
[BLE] Discovered target: BT-TH-7D7CDD6A [2c:6b:7d:7c:dd:6a]
[BLE] Connecting to BT-TH-7D7CDD6A [2c:6b:7d:7c:dd:6a]...
```


The new controller label shows a Renogy 60A model in the RCC60RVRE family, and Renogy documents BT-2 monitoring support for RCC60RVRE.

If you reused the same BT-2, the ESP32 should still discover:

```
1. BT-TH-E72E9AF5
2. 7c:72:e7:2e:9a:f5
```
Your screenshot should confirm if the adapter is advertising. Most likely, the Renogy phone app connected to it and stopped it from being available to the ESP32.

Do this:

- Force-close the Renogy app.
- Turn off Bluetooth on the phone.
- Power-cycle the BT-2/new controller.
- Restart the ESP32.
- Check Telnet again.

No discovery-filter code changes should be needed. Your filter already matches both:

C++
```
name.startsWith("BT-TH-")
```
and:

C++
```
mac.equalsIgnoreCase("7c:72:e7:2e:9a:f5")
```
Once discovered, the new controller may use a different register map. First verify that it returns the expected 73-byte response before trusting the Rover-derived offsets.
