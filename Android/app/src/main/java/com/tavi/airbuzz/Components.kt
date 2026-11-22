package com.tavi.airbuzz

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun StatusPill() {
    val (text, color) = when {
        !AirStatus.bluetoothOn -> AirStatus.bluetooth to Failed
        AirStatus.linked -> "Fitbit Air ready" to Ready
        AirStatus.bandFound -> "Connecting" to Busy
        else -> "Looking for Fitbit Air" to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Row(
        Modifier.clip(CircleShape).background(MaterialTheme.colorScheme.surfaceVariant)
            .padding(horizontal = 14.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Box(Modifier.size(9.dp).clip(CircleShape).background(color))
        Text(text, fontSize = 14.sp, fontWeight = FontWeight.Medium)
    }
}

/** The hero button: rings ripple outward while the band is being buzzed. */
@Composable
fun PulseButton(running: Boolean, onClick: () -> Unit) {
    val ink = MaterialTheme.colorScheme.onSurface
    val transition = rememberInfiniteTransition(label = "pulse")
    val phase by transition.animateFloat(
        initialValue = 0f, targetValue = 1f, label = "ring",
        animationSpec = infiniteRepeatable(tween(1600, easing = LinearEasing), RepeatMode.Restart),
    )
    Box(
        Modifier.size(300.dp).clip(CircleShape).clickable(
            enabled = !running, interactionSource = MutableInteractionSource(), indication = null, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(300.dp)) {
            val base = size.minDimension * 0.30f
            if (running) {
                for (i in 0 until 3) {
                    val t = (phase + i / 3f) % 1f
                    drawCircle(ink.copy(alpha = (1f - t) * 0.35f), radius = base + (size.minDimension / 2 - base) * t,
                        center = center, style = Stroke(width = 2.5f))
                }
            } else {
                drawCircle(ink.copy(alpha = 0.08f), radius = base * 1.25f, center = center)
            }
            drawCircle(
                Brush.verticalGradient(listOf(InkTop, Ink), startY = center.y - base, endY = center.y + base),
                radius = base, center = Offset(center.x, center.y))
        }
        Text(if (running) "Buzzing" else "Buzz", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
    }
}

@Composable
fun ModeToggle(mode: BuzzMode, onChange: (BuzzMode) -> Unit) {
    Row(
        Modifier.clip(CircleShape).background(MaterialTheme.colorScheme.surfaceVariant).padding(4.dp),
    ) {
        Segment("Single", mode == BuzzMode.SINGLE) { onChange(BuzzMode.SINGLE) }
        Segment("Double", mode == BuzzMode.DOUBLE) { onChange(BuzzMode.DOUBLE) }
    }
}

@Composable
private fun Segment(title: String, selected: Boolean, onClick: () -> Unit) {
    val fill = if (selected) MaterialTheme.colorScheme.onSurface else Color.Transparent
    val text = if (selected) MaterialTheme.colorScheme.surface else MaterialTheme.colorScheme.onSurface
    Box(
        Modifier.width(94.dp).height(36.dp).clip(CircleShape).background(fill)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { Text(title, color = text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold) }
}

@Composable
fun WideButton(title: String, filled: Boolean = false, onClick: () -> Unit) {
    val fill = if (filled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.surfaceVariant
    val text = if (filled) MaterialTheme.colorScheme.surface else MaterialTheme.colorScheme.onSurface
    Box(
        Modifier.fillMaxWidth().clip(CircleShape).background(fill).clickable(onClick = onClick).padding(vertical = 15.dp),
        contentAlignment = Alignment.Center,
    ) { Text(title, color = text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold) }
}
