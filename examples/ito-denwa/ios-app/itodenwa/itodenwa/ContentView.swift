import SwiftUI

struct ContentView: View {
    @StateObject private var ble = BluetoothProvisionManager()
    @State private var ssid = "mx-free24"
    @State private var password = "ckuv7482"
    @State private var deviceID = "aaaa"

    var body: some View {
        NavigationView {
            Form {
                Section("Device") {
                    statusRow
                }

                if ble.state == .idle {
                    Section {
                        Button {
                            ble.startScan()
                        } label: {
                            HStack {
                                Spacer()
                                Label("Scan for Device", systemImage: "antenna.radiowaves.left.and.right")
                                    .font(.headline)
                                Spacer()
                            }
                        }
                    }
                }

                if isConnected || isInProgress {
                    Section("Wi-Fi Credentials") {
                        TextField("SSID", text: $ssid)
                            .autocapitalization(.none)
                            .disableAutocorrection(true)
                        SecureField("Password", text: $password)
                        TextField("Device ID", text: $deviceID)
                            .autocapitalization(.none)
                            .disableAutocorrection(true)
                    }
                    .disabled(ble.state != .ready)

                    Section("Progress") {
                        checkRow("BLE Connected", done: isConnected)
                        checkRow("SSID Written", done: ble.ssidOK)
                        checkRow("Password Written", done: ble.passOK)
                        checkRow("Device ID Written", done: ble.devidOK)
                        checkRow("Saved to Flash", done: ble.commitDone)
                    }

                    Section {
                        if ble.state == .ready {
                            Button("Provision") {
                                ble.provision(ssid: ssid, password: password, deviceID: deviceID)
                            }
                            .disabled(ssid.isEmpty || password.isEmpty || deviceID.isEmpty)
                        } else if ble.state == .saved {
                            Label("Provisioning Complete!", systemImage: "checkmark.seal.fill")
                                .foregroundColor(.green)

                            Button("Start Over") {
                                ssid = ""
                                password = ""
                                deviceID = ""
                                ble.disconnect()
                            }
                        }
                    }
                }
            }
            .navigationTitle("Ito-Denwa Setup")
        }
        .navigationViewStyle(.stack)
    }

    // MARK: - Helpers

    private var isConnected: Bool {
        switch ble.state {
        case .idle, .scanning, .connecting, .discovering, .failed:
            return false
        default:
            return true
        }
    }

    private var isInProgress: Bool {
        switch ble.state {
        case .idle, .failed:
            return false
        default:
            return true
        }
    }

    @ViewBuilder
    private var statusRow: some View {
        switch ble.state {
        case .idle:
            Label("Tap Scan to begin", systemImage: "bolt.horizontal.circle")
        case .scanning:
            HStack { ProgressView(); Text("Scanning...") }
        case .connecting:
            HStack { ProgressView(); Text("Connecting...") }
        case .discovering:
            HStack { ProgressView(); Text("Discovering services...") }
        case .ready:
            Label("Connected — enter credentials", systemImage: "checkmark.circle.fill")
                .foregroundColor(.green)
        case .provisioning:
            HStack { ProgressView(); Text("Provisioning...") }
        case .saved:
            Label("Saved! Device will reboot.", systemImage: "checkmark.seal.fill")
                .foregroundColor(.green)
        case .failed(let msg):
            VStack(alignment: .leading) {
                Label("Error", systemImage: "xmark.octagon.fill")
                    .foregroundColor(.red)
                Text(msg).font(.caption).foregroundColor(.secondary)
                Button("Retry") {
                    ble.disconnect()
                    ble.startScan()
                }
            }
        }
    }

    private func checkRow(_ title: String, done: Bool) -> some View {
        HStack {
            Image(systemName: done ? "checkmark.square.fill" : "square")
                .foregroundColor(done ? .green : .secondary)
            Text(title)
        }
    }
}

#Preview {
    ContentView()
}
