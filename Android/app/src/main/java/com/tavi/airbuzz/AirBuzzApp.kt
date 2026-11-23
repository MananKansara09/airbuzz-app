package com.tavi.airbuzz

import android.app.Application

class AirBuzzApp : Application() {
    override fun onCreate() {
        super.onCreate()
        NotificationHistory.init(this)
        AirLink.get(this)
    }
}
