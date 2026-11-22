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
 */
class AirBuzzListener : NotificationListenerService() {
    private val seen = HashSet<String>()
    private var lastBuzz = 0L

    override fun onCreate() {
        super.onCreate()
        AirLink.get(this)
    }

    override fun onListenerConnected() {
        AirLink.get(this).warmUp()
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        seen.remove(sbn.key)
    }

    override fun onNotificationPosted(sbn: StatusBarNotification, rankingMap: RankingMap) {
        if (!shouldBuzz(sbn, rankingMap)) return
        val link = AirLink.get(this)
        // A burst of messages needs one pulse, not a queue of them.
        if (link.pending >= 2) return
        val now = SystemClock.elapsedRealtime()
        if (now - lastBuzz < MIN_GAP_MS) return
        lastBuzz = now
        link.buzz(Prefs.mode(this)) { }
    }

    private fun shouldBuzz(sbn: StatusBarNotification, rankingMap: RankingMap): Boolean {
        if (sbn.packageName == packageName) return false
        if (!Prefs.allApps(this) && sbn.packageName !in Prefs.apps(this)) return false
        val notification = sbn.notification
        val isCall = notification.category == Notification.CATEGORY_CALL
        val flags = notification.flags
        if (flags and Notification.FLAG_GROUP_SUMMARY != 0) return false
        if (!isCall && flags and (Notification.FLAG_ONGOING_EVENT or Notification.FLAG_FOREGROUND_SERVICE) != 0) return false
        // Updates to a notification already shown should not buzz again.
        val firstTime = seen.add(sbn.key)
        if (!firstTime && (isCall || flags and Notification.FLAG_ONLY_ALERT_ONCE != 0)) return false
        // Respect what the phone itself would alert for: silent channels and Do Not Disturb.
        val ranking = Ranking()
        if (rankingMap.getRanking(sbn.key, ranking)) {
            if (ranking.importance < NotificationManager.IMPORTANCE_DEFAULT) return false
            if (!ranking.matchesInterruptionFilter()) return false
        }
        return true
    }

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
