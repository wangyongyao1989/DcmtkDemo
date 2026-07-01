package com.example.dcmtk.jni;

import com.example.dcmtk.callback.ProgressCallback;

import java.util.HashMap;

public class DcmtkJni {

    static {
        System.loadLibrary("dcmtk_native");
    }

    public native String stringFromJNI();

    public static native void initDcmtk(String dictPath);

    public static native HashMap<String, String> loadDicomFileInfo(String filePath);

    public static native boolean writeDicomFile(String rawDataPath, String destDcmPath, int width, int height);

    public static native boolean connectPACS(String host, int port, String localAET, String remoteAET);

    public static native boolean cEcho(String host, int port, String localAET, String remoteAET);

    public static native boolean cStore(String host, int port, String localAET, String remoteAET
            , String dcmPath, ProgressCallback callback);

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
