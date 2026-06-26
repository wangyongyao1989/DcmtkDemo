package com.example.dcmtkdemo.viewmodel;

import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

import com.example.dcmtkdemo.model.PatientRecord;

import java.util.ArrayList;
import java.util.List;

public class PacsViewModel extends ViewModel {
    public final MutableLiveData<String> host = new MutableLiveData<>("192.168.10.153");
    public final MutableLiveData<Integer> port = new MutableLiveData<>(11112);
    public final MutableLiveData<String> localAet = new MutableLiveData<>("ANDROID_SCU");
    public final MutableLiveData<String> remoteAet = new MutableLiveData<>("ACME_STORE");

    public final MutableLiveData<Boolean> assetsReady = new MutableLiveData<>(false);

    public final MutableLiveData<List<PatientRecord>> queryResults = new MutableLiveData<>(new ArrayList<>());
}
