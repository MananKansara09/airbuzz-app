package com.tavi.airbuzz

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.os.ParcelUuid

/** Locates the band: a bonded device first, then a scan for the tracker service. */
@SuppressLint("MissingPermission")
internal class AirBandFinder(private val link: AirLink) {
    private var scanning = false
    private var scanDeadline: Runnable? = null

    /**
     * The band is normally bonded to this phone through Google Health and may not
     * advertise while that app holds it, so a bonded device is the fast path.
     */
    fun retrieveBand(): Boolean {
        val adapter = link.adapter ?: return false
        val saved = Prefs.bandAddress(link.context)
        val bonded = adapter.bondedDevices.orEmpty()
        val found = bonded.firstOrNull { it.address == saved }
            ?: bonded.firstOrNull { it.name?.contains("fitbit", ignoreCase = true) == true }
            ?: return false
        select(found)
        return true
    }

    private fun select(found: BluetoothDevice) {
        stopScan()
        link.device = found
        Prefs.setBandAddress(link.context, found.address)
        val description = "Band found: ${found.name ?: "Fitbit Air"}"
        AirStatus.band = description
        link.publish(description)
    }

    fun scan(request: AirLink.Request) {
        if (scanning) return
        val scanner = link.adapter?.bluetoothLeScanner
        if (scanner == null) {
            link.finish(Result.failure(AirFailure("Turn on Bluetooth to reach your Fitbit Air.")))
            return
        }
        link.publish("Scanning for Fitbit Air")
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(AirLink.SERVICE)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanning = true
        scanner.startScan(listOf(filter), settings, callback)
        val timeout = Runnable {
            if (link.active === request && link.device == null) {
                link.finish(Result.failure(AirFailure("No Fitbit Air found. Keep the band nearby and try again.")))
            }
        }
        scanDeadline = timeout
        link.handler.postDelayed(timeout, 10_000)
    }

    fun stopScan() {
        scanDeadline?.let(link.handler::removeCallbacks)
        if (!scanning) return
        scanning = false
        try { link.adapter?.bluetoothLeScanner?.stopScan(callback) } catch (_: RuntimeException) {}
    }

    private val callback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            link.handler.post {
                if (link.active == null || link.device != null) return@post
                select(result.device)
                link.ensureLink()
            }
        }

        override fun onScanFailed(errorCode: Int) {
            link.handler.post {
                scanning = false
                if (link.active != null) link.finish(Result.failure(AirFailure("Bluetooth scan failed ($errorCode).")))
            }
        }
    }
}
