package com.tavi.airbuzz

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext

private enum class Screen { MAIN, SETUP, LOG }

/** Bumped on every resume so screens re-check permissions the user changed in Settings. */
object AppEvents {
    var resumes by mutableIntStateOf(0)
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { AirBuzzTheme { Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) { Root() } } }
    }

    override fun onResume() {
        super.onResume()
        AppEvents.resumes++
        ListenerAccess.rebind(this)
        if (Permissions.granted(this)) AirLink.get(this).warmUp()
    }
}

@Composable
private fun Root() {
    val context = LocalContext.current
    var screen by rememberSaveable { mutableStateOf(Screen.MAIN) }
    val askPermissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        AirLink.get(context).warmUp()
    }
    BackHandler(enabled = screen != Screen.MAIN) { screen = Screen.MAIN }
    when (screen) {
        Screen.MAIN -> MainScreen(
            onSetup = { screen = Screen.SETUP },
            onLog = { screen = Screen.LOG },
            onAllowBluetooth = { askPermissions.launch(Permissions.missing(context).toTypedArray()) },
        )
        Screen.SETUP -> NotificationsScreen(onBack = { screen = Screen.MAIN })
        Screen.LOG -> ActivityLogScreen(onBack = { screen = Screen.MAIN })
    }
}
