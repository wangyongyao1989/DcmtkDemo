package com.example.dcmtk.viewmodel

import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel
import com.example.dcmtk.model.PatientRecord

class PacsViewModel : ViewModel() {
    val host = MutableLiveData("192.168.10.153")
    val port = MutableLiveData(11112)
    val localAet = MutableLiveData("ANDROID_SCU")
    val remoteAet = MutableLiveData("ACME_STORE")

    val isPacsConnected = MutableLiveData(false)
    val isCEchoSuccess = MutableLiveData(false)

    val assetsReady = MutableLiveData(false)

    val queryResults = MutableLiveData<List<PatientRecord>>(ArrayList())
    val mwlResults = MutableLiveData<List<PatientRecord>>(ArrayList())
}
