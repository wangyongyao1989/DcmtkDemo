package com.example.dcmtkdemo;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.widget.TextView;

import com.example.dcmtkdemo.databinding.ActivityMainBinding;

public class MainActivity extends AppCompatActivity {



    private ActivityMainBinding binding;
    private DcmtkJni dcmtkJni;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        dcmtkJni = new DcmtkJni();
        // Example of a call to a native method
        TextView tv = binding.sampleText;
        tv.setText(dcmtkJni.stringFromJNI());
    }






}