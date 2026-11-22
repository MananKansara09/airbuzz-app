import Foundation
@preconcurrency import CoreBluetooth

extension AirLink: CBCentralManagerDelegate, CBPeripheralDelegate {
    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        guard let band = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first else { return }
        peripheral = band
        band.delegate = self
        let service = band.services?.first { $0.uuid == Self.serviceID }
        tx = service?.characteristics?.first { $0.uuid == Self.txID }
        rx = service?.characteristics?.first { $0.uuid == Self.rxID }
        log("Restored band link")
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let state: String = switch central.state {
        case .poweredOn: "Bluetooth on"
        case .poweredOff: "Bluetooth off"
        case .unauthorized: "Bluetooth access denied"
        case .unsupported: "Bluetooth unavailable"
        case .resetting: "Bluetooth resetting"
        case .unknown: "Bluetooth starting"
        @unknown default: "Bluetooth state unknown"
        }
        Task { @MainActor in AirStatus.shared.bluetooth = state }
        log(state)
        if active != nil, central.state != .poweredOn, session != nil {
            AB_Session_LinkLost(session)
        } else if active != nil {
            findBand()
        } else if central.state == .poweredOn {
            ensureLink()
        } else {
            resetLink()
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover band: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard active != nil, peripheral == nil else { return }
        connect(band)
    }

    func centralManager(_ central: CBCentralManager, didConnect band: CBPeripheral) {
        guard band === peripheral else { return }
        if active != nil { publish("Connected, discovering tracker service") }
        if let rx, tx != nil {
            if rx.isNotifying { markReady() } else { band.setNotifyValue(true, for: rx) }
        } else {
            band.discoverServices([Self.serviceID])
        }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect band: CBPeripheral, error: Error?) {
        guard band === peripheral else { return }
        if awaitingDisconnect { releaseConnection(); return }
        if active != nil {
            finish(.failure(AirFailure(message: error?.localizedDescription ?? "Could not connect to Fitbit Air.")))
        } else {
            log("Band link failed: \(error?.localizedDescription ?? "unknown")")
            peripheral = nil
            resetLink()
        }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral band: CBPeripheral, error: Error?) {
        guard band === peripheral else { return }
        log("Band disconnected")
        if awaitingDisconnect { releaseConnection(); return }
        resetLink()
        if active != nil {
            if let session {
                AB_Session_LinkLost(session)
            } else {
                finish(.failure(AirFailure(message: error?.localizedDescription ?? "The band disconnected.")))
            }
        } else {
            // Pending connect: iOS reconnects when the band comes back.
            central.connect(band)
        }
    }

    func peripheral(_ band: CBPeripheral, didDiscoverServices error: Error?) {
        guard band === peripheral else { return }
        if let error { linkFailed(error.localizedDescription); return }
        guard let service = band.services?.first(where: { $0.uuid == Self.serviceID }) else {
            linkFailed("The Fitbit tracker service was not found.")
            return
        }
        band.discoverCharacteristics([Self.txID, Self.rxID], for: service)
    }

    func peripheral(_ band: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard band === peripheral else { return }
        if let error { linkFailed(error.localizedDescription); return }
        tx = service.characteristics?.first { $0.uuid == Self.txID }
        rx = service.characteristics?.first { $0.uuid == Self.rxID }
        guard let tx, tx.properties.contains(.writeWithoutResponse),
              let rx, rx.properties.contains(.notify) else {
            linkFailed("The band's tracker characteristics are unavailable.")
            return
        }
        if active != nil { publish("Enabling band notifications") }
        if rx.isNotifying { markReady() } else { band.setNotifyValue(true, for: rx) }
    }

    func peripheral(_ band: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard band === peripheral, characteristic.uuid == Self.rxID else { return }
        if let error { linkFailed(error.localizedDescription); return }
        guard characteristic.isNotifying else {
            linkReady = false
            if active == nil { return }
            if let session { AB_Session_LinkLost(session) }
            else { finish(.failure(AirFailure(message: "The band did not enable notifications."))) }
            return
        }
        markReady()
    }

    func peripheral(_ band: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        // While idle the warm link sees Google Health traffic; ignore it.
        guard band === peripheral, active != nil, characteristic.uuid == Self.rxID else { return }
        if let error {
            log(error.localizedDescription)
            if let session { AB_Session_LinkLost(session) }
            else { finish(.failure(error)) }
            return
        }
        guard let data = characteristic.value, let session else { return }
        data.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            AB_Session_Receive(session, base, bytes.count)
        }
    }

    func peripheralIsReady(toSendWriteWithoutResponse band: CBPeripheral) {
        guard band === peripheral, active != nil else { return }
        flushPackets()
    }

    private func linkFailed(_ message: String) {
        if active != nil {
            finish(.failure(AirFailure(message: message)))
        } else {
            log("Band link: \(message)")
            resetLink()
        }
    }
}
