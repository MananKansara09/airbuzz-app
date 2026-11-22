import Foundation
import Observation

@MainActor @Observable
final class AirStatus {
    static let shared = AirStatus()
    struct LogLine: Identifiable {
        let id = UUID()
        let date = Date()
        let text: String
    }
    var bluetooth = "Bluetooth starting"
    var band = "No band found yet"
    var status = "Ready to find your Fitbit Air"
    var linked = false
    var lastResult: String?
    var lastResultDate: Date?
    var lastSeconds: Double?
    var lines: [LogLine] = []

    var bluetoothOn: Bool { bluetooth == "Bluetooth on" }
    var bandFound: Bool { band.hasPrefix("Band found") }

    func append(_ line: String) {
        lines.append(LogLine(text: line))
        if lines.count > 50 { lines.removeFirst(lines.count - 50) }
    }

    func recordResult(_ result: String, seconds: Double?) {
        lastResult = result
        lastResultDate = Date()
        lastSeconds = seconds
    }
}
