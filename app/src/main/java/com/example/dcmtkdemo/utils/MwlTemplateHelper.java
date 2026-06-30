package com.example.dcmtkdemo.utils;

import android.content.Context;
import android.util.Log;

import com.example.dcmtkdemo.jni.DcmtkJni;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class MwlTemplateHelper {
    private static final String TAG = "MwlTemplateHelper";
    private static final String TEMPLATE_ASSETS_DIR = "wlistqry";
    private static final String TEMPLATE_LOCAL_DIR = "mwl_templates";
    private static final String EXPORT_DIR = "mwl_exports";

    public static void prepareTemplates(Context context) {
        File targetDir = new File(context.getFilesDir(), TEMPLATE_LOCAL_DIR);
        if (!targetDir.exists()) {
            targetDir.mkdirs();
        }

        try {
            String[] files = context.getAssets().list(TEMPLATE_ASSETS_DIR);
            if (files != null) {
                for (String fileName : files) {
                    copyAssetToFile(context, TEMPLATE_ASSETS_DIR + "/"
                            + fileName, new File(targetDir, fileName));
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "Failed to list assets in " + TEMPLATE_ASSETS_DIR, e);
        }
    }

    private static void copyAssetToFile(Context context, String assetPath, File targetFile) {
        if (targetFile.exists()) return;

        try (InputStream is = context.getAssets().open(assetPath);
             FileOutputStream fos = new FileOutputStream(targetFile)) {
            byte[] buffer = new byte[8192];
            int read;
            while ((read = is.read(buffer)) != -1) {
                fos.write(buffer, 0, read);
            }
            Log.d(TAG, "Copied template to: " + targetFile.getAbsolutePath());
        } catch (IOException e) {
            Log.e(TAG, "Failed to copy asset: " + assetPath, e);
        }
    }

    public static String[] executeMwlQuery(Context context, String host, int port
            , String localAet, String remoteAet, String templateName) {
        File templateFile = new File(context.getFilesDir(), TEMPLATE_LOCAL_DIR + "/" + templateName);
        if (!templateFile.exists()) {
            Log.e(TAG, "Template file not found: " + templateFile.getAbsolutePath());
            return new String[0];
        }

        File exportDir = new File(context.getFilesDir(), EXPORT_DIR);
        if (!exportDir.exists()) {
            exportDir.mkdirs();
        }

        return DcmtkJni.cFindMWLByTemplate(host, port, localAet, remoteAet
                , templateFile.getAbsolutePath(), exportDir.getAbsolutePath());
    }

    public static String getExportDirPath(Context context) {
        File exportDir = new File(context.getFilesDir(), EXPORT_DIR);
        if (!exportDir.exists()) {
            exportDir.mkdirs();
        }
        return exportDir.getAbsolutePath();
    }
}
