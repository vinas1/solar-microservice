## MacOS Bluetooth Scanner
swift - << 'EOF'
import CoreBluetooth
import Foundation

class Scanner: NSObject, CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            print("Scanning for local BLE devices for 15 seconds...")
            central.scanForPeripherals(withServices: nil, options: nil)
        } else {
            print("Bluetooth power state: \(central.state.rawValue)")
        }
    }
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        let name = peripheral.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? "(No Name)"
        print("RSSI: \(RSSI) dBm | Name: \(name) | UUID: \(peripheral.identifier)")
    }
}

let scanner = Scanner()
let manager = CBCentralManager(delegate: scanner, queue: nil)
RunLoop.main.run(until: Date().addingTimeInterval(15))
EOF