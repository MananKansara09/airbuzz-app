package com.tavi.airbuzz

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

class AirFailure(message: String) : Exception(message)

fun interface Cancellable { fun cancel() }

/**
 * BLE transport for the shared C core. Every field below belongs to [handler]'s
 * thread. GATT and core callbacks copy their data and hop onto it.
 */
@SuppressLint("MissingPermission")
class AirLink private constructor(internal val context: Context) {
    companion object {
        val SERVICE: UUID = UUID.fromString("abbaff00-e56a-484c-b832-8b17cf6cbfe8")
        val TX: UUID = UUID.fromString("abbaff01-e56a-484c-b832-8b17cf6cbfe8")
        val RX: UUID = UUID.fromString("abbaff02-e56a-484c-b832-8b17cf6cbfe8")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val DEFAULT_MTU = 23
        const val WANTED_MTU = 247

        @Volatile private var instance: AirLink? = null
        fun get(context: Context): AirLink = instance ?: synchronized(this) {
            instance ?: AirLink(context.applicationContext).also { instance = it }
        }
    }

    class Request(val id: Long, val op: AirNative.Op, val callback: (Result<Int>) -> Unit)

    internal val handler: Handler
    private val ids = AtomicLong()
    private val pendingCount = AtomicInteger()

    /** Requests accepted but not yet finished, for callers that coalesce bursts. */
    val pending: Int get() = pendingCount.get()

    internal val requests = ArrayDeque<Request>()
    internal var active: Request? = null
    internal var adapter: BluetoothAdapter? = null
    internal var device: BluetoothDevice? = null
    internal var gatt: BluetoothGatt? = null
    internal var connected = false
    internal var tx: BluetoothGattCharacteristic? = null
    internal var rx: BluetoothGattCharacteristic? = null
    internal var notifying = false
    internal var linkReady = false
    internal var mtu = DEFAULT_MTU
    internal val packets = ArrayDeque<ByteArray>()
    internal var writing = false
    internal var session = 0L
    internal var deadline: Runnable? = null
    internal var disconnectDeadline: Runnable? = null
    internal var abortResult: Result<Int>? = null
    internal var awaitingDisconnect = false
    internal val finder = AirBandFinder(this)
    internal var startedAt = 0L
    internal var mtuRequested = false

    internal val isWarm: Boolean
        get() = linkReady && connected && tx != null && notifying

    init {
        val thread = HandlerThread("airbuzz-ble").also { it.start() }
        handler = Handler(thread.looper)
        handler.post {
            adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
            AirBluetoothState.register(this)
            ensureLink()
        }
    }

    fun buzz(mode: BuzzMode, callback: (Result<Int>) -> Unit): Cancellable =
        enqueue(if (mode == BuzzMode.SINGLE) AirNative.Op.TAP else AirNative.Op.DOUBLE, callback)

    fun readIntensity(callback: (Result<Int>) -> Unit): Cancellable = enqueue(AirNative.Op.READ, callback)

    /** Opens the idle link once permissions exist, so the first buzz skips setup. */
    fun warmUp() { handler.post { AirBluetoothState.refresh(this); ensureLink() } }

    private fun enqueue(op: AirNative.Op, callback: (Result<Int>) -> Unit): Cancellable {
        val request = Request(ids.incrementAndGet(), op) { result ->
            pendingCount.decrementAndGet()
            callback(result)
        }
        pendingCount.incrementAndGet()
        handler.post {
            requests.addLast(request)
            startNext()
        }
        return Cancellable {
            handler.post {
                if (active === request) finish(Result.failure(CancellationException()))
                else if (requests.remove(request)) request.callback(Result.failure(CancellationException()))
            }
        }
    }

    internal fun startNext() {
        if (active != null || awaitingDisconnect || requests.isEmpty()) return
        val request = requests.removeFirst()
        active = request
        val timeout = Runnable {
            if (active === request) finish(Result.failure(AirFailure("The band did not finish within 25 seconds. Try again.")))
        }
        deadline = timeout
        handler.postDelayed(timeout, 25_000)
        startedAt = SystemClock.elapsedRealtime()
        publish(if (isWarm) "Link already open" else "Preparing Bluetooth")
        findBand()
    }

    internal fun findBand() {
        val request = active ?: return
        val adapter = adapter
        if (adapter == null) {
            finish(Result.failure(AirFailure("This device does not support Bluetooth LE")))
            return
        }
        if (!Permissions.granted(context)) {
            finish(Result.failure(AirFailure("Allow Bluetooth access for AirBuzz in Settings.")))
            return
        }
        if (!adapter.isEnabled) {
            finish(Result.failure(AirFailure("Turn on Bluetooth to reach your Fitbit Air.")))
            return
        }
        if (device != null || finder.retrieveBand()) {
            if (isWarm) beginSession() else ensureLink()
            return
        }
        finder.scan(request)
    }

    /** Keeps the band connected and subscribed between operations (the warm link). */
    internal fun ensureLink() {
        val adapter = adapter ?: return
        if (!adapter.isEnabled || !Permissions.granted(context) || awaitingDisconnect) return
        if (device == null && !finder.retrieveBand()) return
        val band = device ?: return
        val current = gatt
        when {
            current == null -> {
                connected = false
                gatt = band.connectGatt(context, false, AirGattCallback(this), BluetoothDevice.TRANSPORT_LE)
            }
            connected && isWarm -> if (active != null) beginSession()
            connected && tx != null && rx != null -> enableNotifications()
            connected -> if (!mtuRequested) { mtuRequested = true; requestMtuOrDiscover() }
        }
    }

    internal fun requestMtuOrDiscover() {
        val current = gatt ?: return
        if (!current.requestMtu(WANTED_MTU)) current.discoverServices()
    }

    internal fun enableNotifications() {
        val current = gatt ?: return
        val characteristic = rx ?: return
        if (notifying) { markReady(); return }
        if (active != null) publish("Enabling band notifications")
        current.setCharacteristicNotification(characteristic, true)
        val descriptor = characteristic.getDescriptor(CCCD)
        if (descriptor == null) { linkFailed("The band's tracker characteristics are unavailable."); return }
        @Suppress("DEPRECATION")
        descriptor.value = BluetoothGattDescriptorValues.ENABLE_NOTIFICATION
        @Suppress("DEPRECATION")
        current.writeDescriptor(descriptor)
    }

    internal fun markReady() {
        linkReady = true
        notifying = true
        log("Band link ready")
        AirStatus.linked = true
        if (active != null) beginSession()
    }

    internal fun resetLink() {
        linkReady = false
        notifying = false
        tx = null
        rx = null
        mtuRequested = false
        AirStatus.linked = false
    }

    internal fun linkFailed(message: String) {
        if (active != null) finish(Result.failure(AirFailure(message)))
        else { log("Band link: $message"); disconnect() }
    }

    internal fun beginSession() {
        val request = active ?: return
        if (session != 0L || !isWarm) return
        val packetSize = mtu - 3
        if (packetSize <= 0) {
            finish(Result.failure(AirFailure("The band did not negotiate a packet size.")))
            return
        }
        val id = request.id
        val handle = AirNative.nativeCreate(object : AirNative.Sink {
            override fun send(packet: ByteArray) {
                handler.post { if (active?.id == id) this@AirLink.send(packet) }
            }

            override fun done(result: Int, intensity: Int, message: String) {
                handler.post {
                    if (active?.id != id) return@post
                    if (result == AirNative.OK) finish(Result.success(intensity), coreDone = true)
                    else finish(Result.failure(AirFailure(message.ifEmpty { "Band operation failed." })), coreDone = true)
                }
            }

            override fun log(line: String) {
                handler.post { if (active?.id == id) this@AirLink.log(line) }
            }
        }, packetSize)
        if (handle == 0L) {
            finish(Result.failure(AirFailure("Could not create the band session.")))
            return
        }
        session = handle
        publish("Talking to band ($packetSize-byte packets)")
        if (AirNative.nativeStart(handle, request.op.code) != AirNative.OK) {
            finish(Result.failure(AirFailure("Could not start the band session.")))
        }
    }

    internal fun send(packet: ByteArray) {
        if (!canTransmit()) return
        if (packet.size > mtu - 3) {
            finish(Result.failure(AirFailure("The core emitted an oversized Bluetooth packet.")))
            return
        }
        if (packets.size >= 512) {
            finish(Result.failure(AirFailure("Bluetooth could not keep up with the band.")))
            return
        }
        packets.addLast(packet)
        flushPackets()
    }

    /** One write in flight at a time; onCharacteristicWrite releases the next. */
    internal fun flushPackets() {
        if (writing || packets.isEmpty() || !canTransmit()) return
        val current = gatt ?: return
        val characteristic = tx ?: return
        if (!connected) return
        @Suppress("DEPRECATION")
        characteristic.value = packets.first()
        characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        @Suppress("DEPRECATION")
        if (current.writeCharacteristic(characteristic)) {
            packets.removeFirst()
            writing = true
        } else {
            handler.postDelayed({ flushPackets() }, 10)
        }
    }

    private fun canTransmit(): Boolean {
        if (session == 0L || AirNative.nativeIsMuted(session)) {
            packets.clear()
            return false
        }
        return true
    }

    internal fun finish(proposed: Result<Int>, coreDone: Boolean = false) {
        val request = active ?: return
        // Leave RX and TX running while the core restores and releases the channel.
        if (session != 0L && !coreDone) {
            if (abortResult != null) return
            abortResult = proposed
            deadline?.let(handler::removeCallbacks)
            AirNative.nativeAbort(session)
            val timeout = Runnable { if (active === request) finish(proposed, coreDone = true) }
            deadline = timeout
            handler.postDelayed(timeout, 3_000)
            return
        }
        val result = abortResult ?: proposed
        val keepWarm = result.isSuccess
        abortResult = null
        active = null
        deadline?.let(handler::removeCallbacks)
        finder.stopScan()
        if (session != 0L) {
            AirNative.nativeDestroy(session)
            session = 0
        }
        packets.clear()
        writing = false
        if (!keepWarm) disconnect()
        val summary = result.fold(
            onSuccess = { "Finished. Strength: ${if (it == 2) "Low" else "High"}" },
            onFailure = { it.message ?: "Band operation failed." })
        publish(summary)
        val seconds = if (startedAt == 0L) null else (SystemClock.elapsedRealtime() - startedAt) / 1000.0
        startedAt = 0
        AirStatus.recordResult(summary, seconds)
        request.callback(result)
        if (!awaitingDisconnect) startNext()
    }

    /** Drops the connection so the next request starts from a clean link. */
    internal fun disconnect() {
        val current = gatt
        if (current == null) { resetLink(); return }
        awaitingDisconnect = true
        resetLink()
        val timeout = Runnable { releaseConnection() }
        disconnectDeadline = timeout
        handler.postDelayed(timeout, 2_000)
        current.disconnect()
    }

    internal fun releaseConnection() {
        disconnectDeadline?.let(handler::removeCallbacks)
        gatt?.close()
        gatt = null
        connected = false
        awaitingDisconnect = false
        resetLink()
        startNext()
    }

    internal fun publish(status: String) {
        log(status)
        AirStatus.status = status
    }

    internal fun log(line: String) {
        val text = if (active != null && startedAt != 0L) "+${SystemClock.elapsedRealtime() - startedAt} ms  $line" else line
        AirStatus.append(text)
    }
}

internal object BluetoothGattDescriptorValues {
    val ENABLE_NOTIFICATION = byteArrayOf(0x01, 0x00)
}
