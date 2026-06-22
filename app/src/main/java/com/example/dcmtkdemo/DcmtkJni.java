package com.example.dcmtkdemo;

import android.graphics.Bitmap;

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
    public static native boolean initDcmtk(String dictPath);

    /**
     * 读取 DICOM 文件的基础信息
     * @param filePath DICOM 文件绝对路径
     * @return 格式化的信息字符串，失败返回错误信息
     */
    public static native String getDicomInfo(String filePath);

    /**
     * 将 DICOM 图像渲染到 Android Bitmap 中
     * 注意：传入的 Bitmap 必须是 ARGB_8888 格式，且宽高与 DICOM 图像行列数一致
     * @param filePath DICOM 文件绝对路径
     * @param bitmap 目标 Bitmap
     * @return 渲染是否成功
     */
    public static native boolean renderDicomToBitmap(String filePath, Bitmap bitmap);


}
