package com.tavi.airbuzz

/** Thin JNI layer over the shared AB_Session C core (Core/ab_session.h). */
object AirNative {
    /** Operation codes mirror AB_Operation. */
    enum class Op(val code: Int) { READ(0), TAP(1), DOUBLE(2) }

    /** Called on the Golden Gate loop thread. Implementations must not block. */
    interface Sink {
        fun send(packet: ByteArray)
        fun done(result: Int, intensity: Int, message: String)
        fun log(line: String)
    }

    init { System.loadLibrary("airbuzz_jni") }

    const val OK = 0

    @JvmStatic external fun nativeCreate(sink: Sink, packetSize: Int): Long
    @JvmStatic external fun nativeReceive(handle: Long, data: ByteArray)
    @JvmStatic external fun nativeLinkLost(handle: Long)
    @JvmStatic external fun nativeStart(handle: Long, operation: Int): Int
    @JvmStatic external fun nativeAbort(handle: Long)
    @JvmStatic external fun nativeIsMuted(handle: Long): Boolean
    @JvmStatic external fun nativeDestroy(handle: Long)
}
