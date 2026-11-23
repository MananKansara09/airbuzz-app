package com.tavi.airbuzz

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.clickable
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.Locale

@Composable
fun MainScreen(onSetup: () -> Unit, onLog: () -> Unit, onHistory: () -> Unit, onAllowBluetooth: () -> Unit) {
    val context = LocalContext.current
    val haptics = LocalHapticFeedback.current
    var running by remember { mutableStateOf(false) }
    var mode by remember { mutableStateOf(Prefs.mode(context)) }
    var menu by remember { mutableStateOf(false) }
    var inFlight by remember { mutableStateOf<Cancellable?>(null) }
    val permitted = AppEvents.resumes >= 0 && Permissions.granted(context)
    // A navigation away must not leave a request holding the band.
    DisposableEffect(Unit) { onDispose { inFlight?.cancel() } }

    fun run(read: Boolean) {
        if (running) return
        if (!permitted) { onAllowBluetooth(); return }
        running = true
        haptics.performHapticFeedback(HapticFeedbackType.LongPress)
        val link = AirLink.get(context)
        val done: (Result<Int>) -> Unit = { running = false; inFlight = null }
        inFlight = if (read) link.readIntensity(done) else link.buzz(mode, done)
    }

    Column(
        Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(horizontal = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Box(Modifier.fillMaxWidth().padding(top = 8.dp)) {
            Box(Modifier.align(Alignment.Center)) { StatusPill() }
            Box(Modifier.align(Alignment.CenterEnd)) {
                Text("...", fontSize = 22.sp, fontWeight = FontWeight.Bold,
                    modifier = Modifier.clickable { menu = true }.padding(12.dp))
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text("Read strength") }, enabled = !running,
                        onClick = { menu = false; run(read = true) })
                    DropdownMenuItem(text = { Text("Notification history") }, onClick = { menu = false; onHistory() })
                    DropdownMenuItem(text = { Text("Activity") }, onClick = { menu = false; onLog() })
                }
            }
        }
        Spacer(Modifier.weight(1f))
        PulseButton(running) { run(read = false) }
        Box(Modifier.height(64.dp).padding(top = 16.dp), contentAlignment = Alignment.Center) { ResultText(running) }
        Spacer(Modifier.weight(1f))
        if (!permitted) {
            WideButton("Allow Bluetooth", filled = true, onClick = onAllowBluetooth)
            Spacer(Modifier.height(16.dp))
        }
        ModeToggle(mode) { mode = it; Prefs.setMode(context, it) }
        Spacer(Modifier.height(20.dp))
        WideButton("Buzz on notifications", onClick = onSetup)
        Spacer(Modifier.height(12.dp))
    }
}

@Composable
private fun ResultText(running: Boolean) {
    val secondary = MaterialTheme.colorScheme.onSurfaceVariant
    val result = AirStatus.lastResult
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(3.dp)) {
        when {
            running -> Text(AirStatus.status, color = secondary, fontSize = 16.sp, textAlign = TextAlign.Center)
            result != null -> {
                Text(result, fontSize = 16.sp, fontWeight = FontWeight.Medium, textAlign = TextAlign.Center)
                AirStatus.lastSeconds?.let {
                    Text(String.format(Locale.US, "%.1f s", it), color = secondary, fontSize = 12.sp)
                }
            }
            else -> Text("Tap to buzz your band", color = secondary, fontSize = 16.sp)
        }
    }
}
