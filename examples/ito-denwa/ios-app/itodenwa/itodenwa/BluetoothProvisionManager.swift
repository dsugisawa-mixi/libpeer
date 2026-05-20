import Foundation
import CoreBluetooth
import Combine
import os

private let log = Logger(subsystem: "jp.co.mixi.itodenwa", category: "BLE")

enum ProvisionState: Equatable {
    case idle
    case scanning
    case connecting
    case discovering
    case ready
    case provisioning
    case saved
    case failed(String)
}

/// Status byte values sent by the device via Notify
private enum DeviceStatus: UInt8 {
    case idle    = 0x00
    case ssidOK  = 0x01
    case passOK  = 0x02
    case devidOK = 0x03
    case saved   = 0x04
    case opusOK  = 0x05
    case error   = 0xFF
}

final class BluetoothProvisionManager: NSObject, ObservableObject {

    // MARK: - UUIDs

    private let serviceUUID = CBUUID(string: "A2D40000-2D11-1AE1-2D80-1A30CCAE0001")
    private let ssidUUID    = CBUUID(string: "A2D40001-2D11-1AE1-2D80-1A30CCAE0001")
    private let passUUID    = CBUUID(string: "A2D40002-2D11-1AE1-2D80-1A30CCAE0001")
    private let commitUUID  = CBUUID(string: "A2D40003-2D11-1AE1-2D80-1A30CCAE0001")
    private let statusUUID  = CBUUID(string: "A2D40004-2D11-1AE1-2D80-1A30CCAE0001")
    private let devidUUID   = CBUUID(string: "A2D40005-2D11-1AE1-2D80-1A30CCAE0001")
    private let opusUUID    = CBUUID(string: "A2D40006-2D11-1AE1-2D80-1A30CCAE0001")

    // MARK: - Published state

    @Published var state: ProvisionState = .idle
    @Published var ssidOK = false
    @Published var passOK = false
    @Published var devidOK = false
    @Published var opusOK = false
    @Published var commitDone = false
    @Published var lastError = ""

    // MARK: - CoreBluetooth

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var ssidChar: CBCharacteristic?
    private var passChar: CBCharacteristic?
    private var commitChar: CBCharacteristic?
    private var statusChar: CBCharacteristic?
    private var devidChar: CBCharacteristic?
    private var opusChar: CBCharacteristic?

    // MARK: - Provisioning queue

    private var pendingSSID = ""
    private var pendingPassword = ""
    private var pendingDeviceID = ""

    /// Steps to execute in order; each step is (data, characteristic, expected notify)
    private var writeQueue: [(Data, CBCharacteristic, DeviceStatus)] = []
    private var retryCount = 0
    private let maxRetries = 3
    private let notifyTimeout: TimeInterval = 2.0
    private var timeoutTimer: Timer?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    // MARK: - Public API

    func startScan() {
        guard central.state == .poweredOn else { return }
        reset()
        state = .scanning
        central.scanForPeripherals(withServices: [serviceUUID], options: nil)
    }

    func provision(ssid: String, password: String, deviceID: String, opusEnabled: Bool) {
        guard state == .ready,
              let ssidChar, let passChar, let devidChar, let opusChar, let commitChar else { return }

        pendingSSID = ssid
        pendingPassword = password
        pendingDeviceID = deviceID
        state = .provisioning

        // Build write queue: SSID → Password → DeviceID → Opus → Commit
        writeQueue = [
            (Data(ssid.utf8),                    ssidChar,   .ssidOK),
            (Data(password.utf8),                passChar,   .passOK),
            (Data(deviceID.utf8),                devidChar,  .devidOK),
            (Data([opusEnabled ? 0x01 : 0x00]),  opusChar,   .opusOK),
            (Data([0x01]),                       commitChar, .saved),
        ]
        retryCount = 0
        sendNext()
    }

    func disconnect() {
        if let p = peripheral {
            central.cancelPeripheralConnection(p)
        }
        reset()
    }

    // MARK: - Write queue engine

    private func sendNext() {
        guard let (data, char, _) = writeQueue.first else { return }
        retryCount = 0
        writeOnce(data: data, char: char)
    }

    private func writeOnce(data: Data, char: CBCharacteristic) {
        let canSend = peripheral?.canSendWriteWithoutResponse ?? false
        let hex = data.map { String(format: "%02x", $0) }.joined()
        log.info("writeOnce: uuid=\(char.uuid), data=\(hex), canSend=\(canSend), retry=\(self.retryCount)")
        peripheral?.writeValue(data, for: char, type: .withoutResponse)
        startTimeout()
    }

    private func startTimeout() {
        timeoutTimer?.invalidate()
        timeoutTimer = Timer.scheduledTimer(withTimeInterval: notifyTimeout, repeats: false) { [weak self] _ in
            self?.handleTimeout()
        }
    }

    private func handleTimeout() {
        guard let (data, char, expected) = writeQueue.first else { return }
        retryCount += 1
        if retryCount >= maxRetries {
            state = .failed("No response for \(expected) after \(maxRetries) retries")
            writeQueue.removeAll()
            return
        }
        // Retry the same write
        writeOnce(data: data, char: char)
    }

    private func handleNotify(_ status: DeviceStatus) {
        guard let (_, _, expected) = writeQueue.first else { return }

        if status == .error {
            timeoutTimer?.invalidate()
            state = .failed("Device rejected the request")
            writeQueue.removeAll()
            return
        }

        guard status == expected else { return }

        timeoutTimer?.invalidate()

        // Mark step complete
        switch status {
        case .ssidOK:  ssidOK = true
        case .passOK:  passOK = true
        case .devidOK: devidOK = true
        case .opusOK:  opusOK = true
        case .saved:   commitDone = true; state = .saved
        default: break
        }

        writeQueue.removeFirst()
        if !writeQueue.isEmpty {
            sendNext()
        }
    }

    private func reset() {
        timeoutTimer?.invalidate()
        state = .idle
        ssidOK = false
        passOK = false
        devidOK = false
        opusOK = false
        commitDone = false
        lastError = ""
        writeQueue.removeAll()
        peripheral = nil
        ssidChar = nil
        passChar = nil
        commitChar = nil
        statusChar = nil
        devidChar = nil
        opusChar = nil
    }
}

// MARK: - CBCentralManagerDelegate

extension BluetoothProvisionManager: CBCentralManagerDelegate {

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state != .poweredOn {
            state = .idle
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        state = .connecting
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager,
                        didConnect peripheral: CBPeripheral) {
        state = .discovering
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        state = .failed(error?.localizedDescription ?? "Connection failed")
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        if state != .saved {
            state = .idle
        }
    }
}

// MARK: - CBPeripheralDelegate

extension BluetoothProvisionManager: CBPeripheralDelegate {

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else {
            state = .failed("Service not found")
            return
        }
        peripheral.discoverCharacteristics(nil, for: service)
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard let chars = service.characteristics else {
            state = .failed("No characteristics found")
            return
        }
        for c in chars {
            switch c.uuid {
            case ssidUUID:   ssidChar = c
            case passUUID:   passChar = c
            case commitUUID: commitChar = c
            case devidUUID:  devidChar = c
            case opusUUID:   opusChar = c
            case statusUUID:
                statusChar = c
                peripheral.setNotifyValue(true, for: c)
            default: break
            }
        }

        var missing: [String] = []
        if ssidChar == nil   { missing.append("SSID") }
        if passChar == nil   { missing.append("Pass") }
        if commitChar == nil { missing.append("Commit") }
        if statusChar == nil { missing.append("Status") }
        if devidChar == nil  { missing.append("DevID") }
        if opusChar == nil   { missing.append("Opus") }

        if missing.isEmpty {
            state = .ready
        } else {
            state = .failed("Missing: \(missing.joined(separator: ", "))")
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        if let error {
            log.error("Notify subscribe failed for \(characteristic.uuid): \(error.localizedDescription)")
        } else {
            log.info("Notify subscribed: \(characteristic.uuid), isNotifying: \(characteristic.isNotifying)")
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        let uuid = characteristic.uuid
        let hex = characteristic.value?.map { String(format: "%02x", $0) }.joined() ?? "nil"
        log.info("didUpdateValue: uuid=\(uuid), data=\(hex), error=\(String(describing: error))")

        guard characteristic.uuid == statusUUID,
              let data = characteristic.value,
              let byte = data.first,
              let status = DeviceStatus(rawValue: byte) else { return }

        DispatchQueue.main.async {
            self.handleNotify(status)
        }
    }
}
