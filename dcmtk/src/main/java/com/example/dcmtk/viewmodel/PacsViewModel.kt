package com.example.dcmtk.viewmodel

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.MutableLiveData
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.model.PatientRecord
import com.example.dcmtk.utils.PacsPrefs

class PacsViewModel(application: Application) : AndroidViewModel(application) {
    val pacsConfig = MutableLiveData(PacsPrefs.getConfig(application))
    val worklistConfig = MutableLiveData(PacsPrefs.getWorklistConfig(application))

    // Backwards compatibility or convenience getters
    val host get() = pacsConfig.value?.host ?: ""
    val port get() = pacsConfig.value?.port ?: 0
    val localAet get() = pacsConfig.value?.localAet ?: ""
    val remoteAet get() = pacsConfig.value?.remoteAet ?: ""

    fun updateConfig(host: String, port: Int, localAet: String, remoteAet: String) {
        pacsConfig.value = PacsConfig(host, port, localAet, remoteAet)
    }

    val isPacsConnected = MutableLiveData(false)
    val isCEchoSuccess = MutableLiveData(false)

    val assetsReady = MutableLiveData(false)

    val queryResults = MutableLiveData<List<PatientRecord>>(ArrayList())
    val mwlResults = MutableLiveData<List<PatientRecord>>(ArrayList())
}
