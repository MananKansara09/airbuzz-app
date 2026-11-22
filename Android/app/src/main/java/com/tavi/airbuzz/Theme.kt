package com.tavi.airbuzz

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/** AirBuzz brand ink (#111111) and the lighter top of its button gradient (#2b2b2b). */
val Ink = Color(0xFF111111)
val InkTop = Color(0xFF2B2B2B)
val Ready = Color(0xFF2FB44F)
val Busy = Color(0xFFF2994A)
val Failed = Color(0xFFE5484D)

private val Light = lightColorScheme(
    primary = Ink, onPrimary = Color.White,
    background = Color.White, onBackground = Ink,
    surface = Color.White, onSurface = Ink,
    surfaceVariant = Color(0xFFF0F0F2), onSurfaceVariant = Color(0xFF6B6B70),
    outlineVariant = Color(0xFFDADADF),
)

private val Dark = darkColorScheme(
    primary = Color.White, onPrimary = Ink,
    background = Color.Black, onBackground = Color.White,
    surface = Color.Black, onSurface = Color.White,
    surfaceVariant = Color(0xFF1C1C1E), onSurfaceVariant = Color(0xFF9A9AA0),
    outlineVariant = Color(0xFF38383A),
)

@Composable
fun AirBuzzTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (isSystemInDarkTheme()) Dark else Light, content = content)
}
