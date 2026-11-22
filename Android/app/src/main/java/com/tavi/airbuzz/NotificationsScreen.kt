package com.tavi.airbuzz

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.toMutableStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private class AppEntry(val label: String, val packageName: String)

private fun launchableApps(context: Context): List<AppEntry> {
    val pm = context.packageManager
    val query = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
    val found = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        pm.queryIntentActivities(query, PackageManager.ResolveInfoFlags.of(0))
    } else {
        @Suppress("DEPRECATION") pm.queryIntentActivities(query, 0)
    }
    return found.map { AppEntry(it.loadLabel(pm).toString(), it.activityInfo.packageName) }
        .filter { it.packageName != context.packageName }
        .distinctBy { it.packageName }
        .sortedBy { it.label.lowercase() }
}

@Composable
fun NotificationsScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    var allApps by remember { mutableStateOf(Prefs.allApps(context)) }
    val chosen = remember { Prefs.apps(context).toMutableStateList() }
    var apps by remember { mutableStateOf<List<AppEntry>?>(null) }
    // Re-check after the user returns from Settings.
    val granted = AppEvents.resumes >= 0 && ListenerAccess.granted(context)
    DisposableEffect(Unit) {
        val loader = Thread { apps = launchableApps(context) }.also { it.start() }
        onDispose { loader.interrupt() }
    }

    Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("Back") }
            Text("Buzz on notifications", fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
        }
        LazyColumn(Modifier.fillMaxSize().padding(horizontal = 24.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            item {
                Text(
                    "AirBuzz watches for notifications from the apps you pick and buzzes the band. " +
                        "It never reads their contents.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.height(20.dp))
                Text(if (granted) "Notification access is on" else "Step 1: allow notification access",
                    fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.height(8.dp))
                if (!granted) {
                    WideButton("Open notification access", filled = true) {
                        context.startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
                    }
                    Spacer(Modifier.height(8.dp))
                    Text("Turn on AirBuzz in the list. Android may ask you to confirm.",
                        color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 13.sp)
                }
                Spacer(Modifier.height(16.dp))
                WideButton("Keep AirBuzz running in background") {
                    context.startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
                }
                Spacer(Modifier.height(4.dp))
                Text("Set AirBuzz to Unrestricted so Android does not stop it while the screen is off.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant, fontSize = 13.sp)
                Spacer(Modifier.height(20.dp))
                Text("Step 2: choose apps", fontWeight = FontWeight.SemiBold)
                SwitchRow("Every app", allApps) { allApps = it; Prefs.setAllApps(context, it) }
            }
            if (!allApps) {
                val list = apps
                if (list == null) item { Text("Loading apps", color = MaterialTheme.colorScheme.onSurfaceVariant) }
                else items(list, key = { it.packageName }) { app ->
                    SwitchRow(app.label, app.packageName in chosen) { on ->
                        Prefs.setApp(context, app.packageName, on)
                        if (on) chosen.add(app.packageName) else chosen.remove(app.packageName)
                    }
                }
            }
            item { Spacer(Modifier.height(24.dp)) }
        }
    }
}

@Composable
private fun SwitchRow(title: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(title, Modifier.weight(1f), maxLines = 1)
        Switch(checked = checked, onCheckedChange = onChange)
    }
}
