package com.tavi.airbuzz

import android.app.Notification
import android.app.NotificationManager
import android.content.ComponentName
import android.content.Context
import android.os.SystemClock
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationManagerCompat

/**
 * Android's counterpart to the iOS Shortcuts automation: the system binds this
 * service once the user grants notification access, and it buzzes the band when
 * a notification from a chosen app arrives. Notification content is never read.
 * Every decision is written to [NotificationHistory].
 */
class AirBuzzListener : NotificationListenerService() {
    private val seen = HashSet<String>()
    private var lastBuzz = 0L

    override fun onCreate() {
        super.onCreate()
        NotificationHistory.init(this)
        AirLink.get(this)
    }

    override fun onListenerConnected() {
        NotificationHistory.add("AirBuzz", packageName, Outcome.EVENT, "Listening for notifications")
        AirLink.get(this).warmUp()
    }

    override fun onListenerDisconnected() {
        NotificationHistory.add("AirBuzz", packageName, Outcome.EVENT, "Android stopped the listener, asking it to restart")
        ListenerAccess.rebind(this)
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        seen.remove(sbn.key)
    }

    override fun onNotificationPosted(sbn: StatusBarNotification, rankingMap: RankingMap) {
        if (sbn.packageName == packageName) return
        val app = appName(sbn.packageName)
        val reason = skipReason(sbn, rankingMap)
        if (reason != null) {
            NotificationHistory.add(app, sbn.packageName, Outcome.SKIPPED, reason)
            return
        }
        val link = AirLink.get(this)
        val now = SystemClock.elapsedRealtime()
        // A burst of messages needs one pulse, not a queue of them.
        if (link.pending >= 2 || now - lastBuzz < MIN_GAP_MS) {
            NotificationHistory.add(app, sbn.packageName, Outcome.SKIPPED, "Merged with a buzz that just happened")
            return
        }
        lastBuzz = now
        val mode = Prefs.mode(this)
        val id = NotificationHistory.add(app, sbn.packageName, Outcome.PENDING, "Buzzing the band")
        link.buzz(mode) { result ->
            result.fold(
                onSuccess = { NotificationHistory.update(id, Outcome.BUZZED, "Band buzzed (${mode.name.lowercase()})") },
                onFailure = { NotificationHistory.update(id, Outcome.FAILED, it.message ?: "Buzz failed") })
        }
    }

    /** Null means the notification should buzz. Otherwise the reason shown in the history. */
    private fun skipReason(sbn: StatusBarNotification, rankingMap: RankingMap): String? {
        if (!Prefs.allApps(this) && sbn.packageName !in Prefs.apps(this)) return "App is not selected"
        val notification = sbn.notification
        val isCall = notification.category == Notification.CATEGORY_CALL
        val flags = notification.flags
        if (flags and Notification.FLAG_GROUP_SUMMARY != 0) return "Group summary"
        if (!isCall && flags and (Notification.FLAG_ONGOING_EVENT or Notification.FLAG_FOREGROUND_SERVICE) != 0) {
            return "Ongoing notification"
        }
        // Updates to a notification already shown should not buzz again.
        val firstTime = seen.add(sbn.key)
        if (!firstTime && (isCall || flags and Notification.FLAG_ONLY_ALERT_ONCE != 0)) return "Update to a notification already shown"
        // Respect what the phone itself would alert for: silent channels and Do Not Disturb.
        val ranking = Ranking()
        if (rankingMap.getRanking(sbn.key, ranking)) {
            if (ranking.importance < NotificationManager.IMPORTANCE_DEFAULT) {
                return "Silent notification (channel importance ${ranking.importance})"
            }
            if (!ranking.matchesInterruptionFilter()) return "Blocked by Do Not Disturb"
        }
        return null
    }

    private fun appName(pkg: String): String = try {
        packageManager.getApplicationLabel(packageManager.getApplicationInfo(pkg, 0)).toString()
    } catch (_: Exception) { pkg }

    private companion object {
        const val MIN_GAP_MS = 1_500L
    }
}

object ListenerAccess {
    fun granted(context: Context): Boolean =
        NotificationManagerCompat.getEnabledListenerPackages(context).contains(context.packageName)

    /** Asks the system to rebind the listener after the app process was killed. */
    fun rebind(context: Context) {
        if (granted(context)) {
            NotificationListenerService.requestRebind(ComponentName(context, AirBuzzListener::class.java))
        }
    }
}
