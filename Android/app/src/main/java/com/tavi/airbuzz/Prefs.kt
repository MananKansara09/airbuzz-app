package com.tavi.airbuzz

import android.content.Context
import android.content.SharedPreferences

/** Small persisted settings: remembered band, buzz mode and the app filter. */
object Prefs {
    private fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences("airbuzz", Context.MODE_PRIVATE)

    fun bandAddress(context: Context): String? = prefs(context).getString("band", null)
    fun setBandAddress(context: Context, address: String?) =
        prefs(context).edit().putString("band", address).apply()

    fun mode(context: Context): BuzzMode =
        if (prefs(context).getString("mode", "single") == "double") BuzzMode.DOUBLE else BuzzMode.SINGLE
    fun setMode(context: Context, mode: BuzzMode) =
        prefs(context).edit().putString("mode", if (mode == BuzzMode.DOUBLE) "double" else "single").apply()

    fun allApps(context: Context): Boolean = prefs(context).getBoolean("all_apps", false)
    fun setAllApps(context: Context, value: Boolean) =
        prefs(context).edit().putBoolean("all_apps", value).apply()

    fun apps(context: Context): Set<String> =
        prefs(context).getStringSet("apps", emptySet())?.toSet() ?: emptySet()
    fun setApp(context: Context, packageName: String, enabled: Boolean) {
        val next = apps(context).toMutableSet()
        if (enabled) next.add(packageName) else next.remove(packageName)
        prefs(context).edit().putStringSet("apps", next).apply()
    }
}
