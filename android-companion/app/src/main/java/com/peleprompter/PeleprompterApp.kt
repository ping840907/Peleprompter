package com.peleprompter

import android.app.Application
import com.peleprompter.util.PrefsManager

/**
 * PeleprompterApp - Application 類別
 *
 * 提供全域的 PrefsManager 實例。
 */
class PeleprompterApp : Application() {

    lateinit var prefsManager: PrefsManager
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        prefsManager = PrefsManager(this)
    }

    companion object {
        lateinit var instance: PeleprompterApp
            private set
    }
}
