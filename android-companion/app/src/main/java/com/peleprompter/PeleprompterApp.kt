package com.peleprompter

import android.app.Application
import com.peleprompter.model.TextDocument
import com.peleprompter.util.PrefsManager

/**
 * PeleprompterApp - Application 類別
 *
 * 提供全域的 PrefsManager 實例與目前載入的文件參照（供 WatchEmulatorActivity 存取）。
 */
class PeleprompterApp : Application() {

    lateinit var prefsManager: PrefsManager
        private set

    /** 目前載入的文件，供 WatchEmulatorActivity 等跨 Activity 元件讀取 */
    var currentDocument: TextDocument? = null

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
