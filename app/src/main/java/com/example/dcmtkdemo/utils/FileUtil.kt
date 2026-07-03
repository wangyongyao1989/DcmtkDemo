package com.example.dcmtkdemo.utils;

import android.content.Context;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class FileUtil {

    public static String copyAssetToExternalStorage(Context context, String assetName, String targetName) throws IOException {
        File dir = context.getExternalFilesDir(null);
        if (dir == null) throw new IOException("External storage not available");
        File file = new File(dir, targetName);
        if (!file.exists()) {
            copyAsset(context, assetName, file);
        }
        return file.getAbsolutePath();
    }

    public static void copyDcmAssetsToExternal(Context context) throws IOException {
        String[] assets = context.getAssets().list("");
        if (assets != null) {
            for (String asset : assets) {
                if (asset.toLowerCase().endsWith(".dcm")) {
                    android.util.Log.d("FileUtil", "Copying asset: " + asset);
                    copyAssetToExternalStorage(context, asset, asset);
                }
            }
        }
    }

    private static void copyAsset(Context context, String assetName, File targetFile) throws IOException {
        try (InputStream is = context.getAssets().open(assetName);
             FileOutputStream fos = new FileOutputStream(targetFile)) {
            byte[] buffer = new byte[1024];
            int read;
            while ((read = is.read(buffer)) != -1) {
                fos.write(buffer, 0, read);
            }
        }
    }
}
