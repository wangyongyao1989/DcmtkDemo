package com.example.dcmtk.viewmodel

import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel
import com.example.dcmtk.model.PacsConfig
import com.example.dcmtk.model.PatientRecord

class PacsViewModel : ViewModel() {
    val pacsConfig = MutableLiveData(PacsConfig("192.168.10.153", 11112, "ANDROID_SCU", "ACME_STORE"))

    // Backwards compatibility or convenience getters (optional, but good for minimal changes in other files)
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
