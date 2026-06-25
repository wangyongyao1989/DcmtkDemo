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
     * 初始化 DCMTK，加载字典文件
     *
     * @param dictPath 字典文件路径
     */
    public static native void initDcmtk(String dictPath);

    /**
     * 加载 DICOM 文件并返回其标签信息
     *
     * @param filePath DICOM 文件的绝对路径
     * @return 包含 Tag 和 Value 的 HashMap
     */
    public static native HashMap<String, String> loadDicomFileInfo(String filePath);

    public static native boolean writeDicomFile(String rawDataPath, String destDcmPath, int width, int height);

    public static native boolean connectPACS(String host, int port, String localAET, String remoteAET);

    public static native boolean cEcho(String host, int port, String localAET, String remoteAET);

    public static native boolean cStore(String host, int port, String localAET, String remoteAET
            , String dcmPath);

    /**
     * @return A list of strings, each being a summary of a found record
     */
    public static native String[] cFind(String host, int port, String localAET, String remoteAET
            , String patientName);

    public static native boolean cMove(String host, int port, String localAET, String remoteAET
            , String patientID, String destAET);

    /**
     * C-GET: Download DICOM files directly to a folder
     */
    public static native boolean cGet(String host, int port, String localAET, String remoteAET
            , String patientID, String saveDir);

    /**
     * 将指定目录下的所有 DICOM 文件转换为 JPG 图片。
     * 转换结果输出到该目录下的 jpg/ 子目录，每个源文件生成一个 <name>.jpg。
     *
     * @param dir 存放 DICOM 文件的目录（即 ../temp）
     * @return 成功转换的文件数量
     */
    public static native int dcmToJpg(String dir);

}
