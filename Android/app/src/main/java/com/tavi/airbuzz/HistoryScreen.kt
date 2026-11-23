package com.tavi.airbuzz

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val stamp = SimpleDateFormat("d MMM, HH:mm:ss", Locale.getDefault())

@Composable
fun HistoryScreen(onBack: () -> Unit) {
    val secondary = MaterialTheme.colorScheme.onSurfaceVariant
    Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("Back") }
            Text("Notification history", Modifier.weight(1f), fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
            TextButton(onClick = { NotificationHistory.clear() }) { Text("Clear") }
        }
        if (NotificationHistory.entries.isEmpty()) {
            Text("No notifications seen yet. If this stays empty after you get notifications, " +
                "notification access is off or Android stopped AirBuzz in the background.",
                Modifier.padding(24.dp), color = secondary)
        } else {
            LazyColumn(Modifier.fillMaxSize().padding(horizontal = 24.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                items(NotificationHistory.entries.reversed(), key = { it.id }) { entry ->
                    val (label, color) = when (entry.outcome) {
                        Outcome.BUZZED -> "Buzzed" to Ready
                        Outcome.FAILED -> "Failed" to Failed
                        Outcome.PENDING -> "Buzzing" to Busy
                        Outcome.SKIPPED -> "Not buzzed" to secondary
                        Outcome.EVENT -> "Info" to secondary
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text(entry.app, fontWeight = FontWeight.SemiBold, maxLines = 1)
                            Text(entry.detail, color = secondary, fontSize = 13.sp)
                        }
                        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text(label, color = color, fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
                            Text(stamp.format(Date(entry.time)), color = secondary, fontSize = 11.sp)
                        }
                    }
                }
            }
        }
    }
}
