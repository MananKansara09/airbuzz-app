package com.tavi.airbuzz

import android.content.Context
import androidx.compose.runtime.mutableStateListOf
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

enum class Outcome { PENDING, BUZZED, FAILED, SKIPPED, EVENT }

/** One notification the listener saw. Only the app is recorded, never the content. */
data class HistoryEntry(
    val id: Long,
    val time: Long,
    val app: String,
    val packageName: String,
    val outcome: Outcome,
    val detail: String,
)

/** Persistent log of what AirBuzz did with each notification, newest last. */
object NotificationHistory {
    private const val MAX = 200
    private val lock = Any()
    private var file: File? = null
    private var nextId = 1L
    val entries = mutableStateListOf<HistoryEntry>()

    fun init(context: Context) = synchronized(lock) {
        if (file != null) return
        val target = File(context.applicationContext.filesDir, "history.json").also { file = it }
        try {
            val array = JSONArray(target.takeIf { it.exists() }?.readText() ?: "[]")
            for (i in 0 until array.length()) {
                val o = array.getJSONObject(i)
                entries.add(HistoryEntry(o.getLong("id"), o.getLong("time"), o.getString("app"),
                    o.getString("pkg"), Outcome.valueOf(o.getString("outcome")), o.getString("detail")))
            }
            nextId = (entries.maxOfOrNull { it.id } ?: 0L) + 1
        } catch (_: Exception) {
            entries.clear()
        }
    }

    fun add(app: String, packageName: String, outcome: Outcome, detail: String): Long = synchronized(lock) {
        val entry = HistoryEntry(nextId++, System.currentTimeMillis(), app, packageName, outcome, detail)
        entries.add(entry)
        if (entries.size > MAX) entries.removeRange(0, entries.size - MAX)
        save()
        entry.id
    }

    fun update(id: Long, outcome: Outcome, detail: String) = synchronized(lock) {
        val index = entries.indexOfFirst { it.id == id }
        if (index >= 0) {
            entries[index] = entries[index].copy(outcome = outcome, detail = detail)
            save()
        }
    }

    fun clear() = synchronized(lock) {
        entries.clear()
        save()
    }

    private fun save() {
        val array = JSONArray()
        for (e in entries) {
            array.put(JSONObject().put("id", e.id).put("time", e.time).put("app", e.app)
                .put("pkg", e.packageName).put("outcome", e.outcome.name).put("detail", e.detail))
        }
        try { file?.writeText(array.toString()) } catch (_: Exception) {}
    }
}
