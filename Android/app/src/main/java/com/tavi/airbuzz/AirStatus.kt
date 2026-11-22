package com.tavi.airbuzz

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

enum class BuzzMode { SINGLE, DOUBLE }

/** Observable UI state. Compose snapshot state accepts writes from any thread. */
object AirStatus {
    class LogLine(val text: String, val time: Long = System.currentTimeMillis())

    var bluetooth by mutableStateOf("Bluetooth starting")
    var band by mutableStateOf("No band found yet")
    var status by mutableStateOf("Ready to find your Fitbit Air")
    var linked by mutableStateOf(false)
    var lastResult by mutableStateOf<String?>(null)
    var lastSeconds by mutableStateOf<Double?>(null)
    val lines = mutableStateListOf<LogLine>()

    val bluetoothOn: Boolean get() = bluetooth == "Bluetooth on"
    val bandFound: Boolean get() = band.startsWith("Band found")

    fun append(line: String) {
        lines.add(LogLine(line))
        if (lines.size > 50) lines.removeRange(0, lines.size - 50)
    }

    fun recordResult(result: String, seconds: Double?) {
        lastResult = result
        lastSeconds = seconds
    }
}
