package com.example.dcmtk.utils

import android.content.Context
import com.example.dcmtk.model.PacsConfig

object PacsPrefs {
    private const val PREFS_NAME = "pacs_prefs"
    private const val KEY_HOST = "pacs_host"
    private const val KEY_PORT = "pacs_port"
    private const val KEY_LOCAL_AET = "pacs_local_aet"
    private const val KEY_REMOTE_AET = "pacs_remote_aet"

    fun saveConfig(context: Context, config: PacsConfig) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit()
            .putString(KEY_HOST, config.host)
            .putInt(KEY_PORT, config.port)
            .putString(KEY_LOCAL_AET, config.localAet)
            .putString(KEY_REMOTE_AET, config.remoteAet)
            .apply()
    }

    fun getConfig(context: Context): PacsConfig {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return PacsConfig(
            host = prefs.getString(KEY_HOST, "192.168.10.110") ?: "192.168.10.153",
            port = prefs.getInt(KEY_PORT, 11112),
            localAet = prefs.getString(KEY_LOCAL_AET, "ANDROID_SCU") ?: "ANDROID_SCU",
            remoteAet = prefs.getString(KEY_REMOTE_AET, "ACME_STORE") ?: "ACME_STORE"
        )
    }
}
