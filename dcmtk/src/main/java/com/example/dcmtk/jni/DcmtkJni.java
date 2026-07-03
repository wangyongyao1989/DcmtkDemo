package com.example.dcmtk.jni;

import android.content.Context;
import android.util.Log;

import com.example.dcmtk.callback.MultiProgressCallback;
import com.example.dcmtk.callback.ProgressCallback;
import com.example.dcmtk.utils.DcmtkFileUtil;

import java.io.IOException;
import java.util.HashMap;

public class DcmtkJni {

    static {
        System.loadLibrary("dcmtk_native");
    }

    public native String stringFromJNI();

    /**
     * 初始化 DCMTK 字典
     * @param context 上下文，用于从 assets 中拷贝 dicom.dic
     */
    public static void initDcmtk(Context context) {
        try {
            String dictPath = DcmtkFileUtil.copyAssetToInternalStorage(context, "dicom.dic");
            initDcmtk(dictPath);
        } catch (IOException e) {
            Log.e("DcmtkJni", "Failed to init dcmtk dictionary", e);
        }
    }

    public static native void initDcmtk(String dictPath);

    public static native HashMap<String, String> loadDicomFileInfo(String filePath);

    public static native boolean writeDicomFile(String rawDataPath, String destDcmPath, int width, int height);

    public static native boolean connectPACS(String host, int port, String localAET, String remoteAET);

    public static native boolean cEcho(String host, int port, String localAET, String remoteAET);

    public static native boolean cStore(String host, int port, String localAET, String remoteAET
            , String dcmPath, ProgressCallback callback);

    public static native int cStoreMulti(String host, int port, String localAET, String remoteAET
            , String[] dcmPaths, MultiProgressCallback callback);

    public static native String[] cFind(String host, int port, String localAET, String remoteAET
            , String patientName);

    public static native String[] cFindByAccession(String host, int port, String localAET, String remoteAET
            , String accessionNumber);

    public static native String[] cFindMWL(String host, int port, String localAET, String remoteAET
            , String modality);

    public static native String[] cFindMWLByTemplate(String host, int port, String localAET, String remoteAET
            , String templatePath, String outputDir);

    public static native boolean cMove(String host, int port, String localAET, String remoteAET
            , String patientID, String destAET);

    public static native boolean cGet(String host, int port, String localAET, String remoteAET
            , String patientID, String saveDir, ProgressCallback callback);

    public static native int dcmToJpg(String dir);

}
