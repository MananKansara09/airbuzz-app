package com.tavi.airbuzz

import android.app.Application

class AirBuzzApp : Application() {
    override fun onCreate() {
        super.onCreate()
        AirLink.get(this)
    }
}
