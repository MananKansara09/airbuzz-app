package com.tavi.airbuzz

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter

/** GATT events arrive on binder threads. Copy what is needed and hop to the link thread. */
@SuppressLint("MissingPermission")
internal class AirGattCallback(private val link: AirLink) : BluetoothGattCallback() {
    private fun onLink(gatt: BluetoothGatt, block: () -> Unit) {
        link.handler.post { if (gatt === link.gatt) block() }
    }

    override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
        onLink(gatt) {
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                link.connected = true
                if (link.active != null) link.publish("Connected, discovering tracker service")
                link.mtuRequested = true
                link.requestMtuOrDiscover()
                return@onLink
            }
            link.log("Band disconnected (status $status)")
            link.connected = false
            if (link.awaitingDisconnect) { link.releaseConnection(); return@onLink }
            link.resetLink()
            link.packets.clear()
            link.writing = false
            gatt.close()
            link.gatt = null
            val session = link.session
            when {
                link.active == null -> {}
                session != 0L -> AirNative.nativeLinkLost(session)
                else -> link.finish(Result.failure(AirFailure(
                    if (status == BluetoothGatt.GATT_SUCCESS) "The band disconnected."
                    else "Could not connect to Fitbit Air (status $status).")))
            }
        }
    }

    override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
        onLink(gatt) {
            link.mtu = if (status == BluetoothGatt.GATT_SUCCESS) mtu else AirLink.DEFAULT_MTU
            gatt.discoverServices()
        }
    }

    override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
        onLink(gatt) {
            if (status != BluetoothGatt.GATT_SUCCESS) { link.linkFailed("Service discovery failed ($status)."); return@onLink }
            val service = gatt.getService(AirLink.SERVICE)
            if (service == null) {
                link.device = null
                Prefs.setBandAddress(link.context, null)
                link.linkFailed("The Fitbit tracker service was not found.")
                return@onLink
            }
            val tx = service.getCharacteristic(AirLink.TX)
            val rx = service.getCharacteristic(AirLink.RX)
            val writable = tx != null && tx.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0
            val notifiable = rx != null && rx.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0
            if (!writable || !notifiable) {
                link.linkFailed("The band's tracker characteristics are unavailable.")
                return@onLink
            }
            link.tx = tx
            link.rx = rx
            link.enableNotifications()
        }
    }

    override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
        if (descriptor.uuid != AirLink.CCCD || descriptor.characteristic.uuid != AirLink.RX) return
        onLink(gatt) {
            if (status == BluetoothGatt.GATT_SUCCESS) link.markReady()
            else link.linkFailed("The band did not enable notifications.")
        }
    }

    override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
        onLink(gatt) {
            if (status != BluetoothGatt.GATT_SUCCESS) link.log("Write failed (status $status)")
            link.writing = false
            link.flushPackets()
        }
    }

    // API 33+ delivers the value directly. Older releases use the deprecated overload.
    override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
        received(gatt, characteristic, value.copyOf())
    }

    @Deprecated("Used below API 33")
    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
        received(gatt, characteristic, characteristic.value?.copyOf() ?: return)
    }

    private fun received(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, data: ByteArray) {
        if (characteristic.uuid != AirLink.RX) return
        // While idle the warm link sees Google Health traffic; ignore it.
        onLink(gatt) {
            if (link.active != null && link.session != 0L) AirNative.nativeReceive(link.session, data)
        }
    }
}

/** Mirrors the adapter state into the UI and the link, like centralManagerDidUpdateState. */
internal object AirBluetoothState {
    fun register(link: AirLink) {
        refresh(link)
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                link.handler.post { refresh(link) }
            }
        }
        link.context.registerReceiver(receiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED))
    }

    fun refresh(link: AirLink) {
        val adapter = link.adapter
        val text = when {
            adapter == null -> "Bluetooth unavailable"
            !Permissions.granted(link.context) -> "Bluetooth access denied"
            else -> when (adapter.state) {
                BluetoothAdapter.STATE_ON -> "Bluetooth on"
                BluetoothAdapter.STATE_OFF -> "Bluetooth off"
                else -> "Bluetooth resetting"
            }
        }
        if (text == AirStatus.bluetooth) return
        AirStatus.bluetooth = text
        link.log(text)
        val on = text == "Bluetooth on"
        when {
            link.active != null && !on && link.session != 0L -> AirNative.nativeLinkLost(link.session)
            link.active != null -> link.findBand()
            on -> link.ensureLink()
            else -> link.resetLink()
        }
    }
}
