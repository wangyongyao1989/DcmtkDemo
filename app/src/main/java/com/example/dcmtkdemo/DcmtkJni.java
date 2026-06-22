package com.example.dcmtkdemo;

import android.graphics.Bitmap;

import java.util.HashMap;

public class DcmtkJni {

    // Used to load the 'dcmtkdemo' library on application startup.
    static {
        System.loadLibrary("dcmtkdemo");
    }


    /**
     * A native method that is implemented by the 'dcmtkdemo' native library,
     * which is packaged with this application.
     */
    public native String stringFromJNI();

    /**
     * 初始化 DCMTK，传入 dicom.dic 字典文件的绝对路径
     * @param dictPath 字典文件路径（通常是拷贝到内部存储后的路径）
     * @return 初始化是否成功
     */
    public static native HashMap<String, String> initDcmtk(String dictPath);

}
