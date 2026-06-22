package com.example.dcmtkdemo;

import android.content.Context;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class FileUtil {
    /**
     * 将 Assets 中的文件拷贝到应用的内部存储目录
     * @param context 上下文
     * @param assetName assets 文件夹中的文件名
     * @return 拷贝后文件的绝对路径
     * @throws IOException 拷贝失败时抛出异常
     */
    public static String copyAssetToInternalStorage(Context context, String assetName) throws IOException {
        File file = new File(context.getFilesDir(), assetName);
        if (!file.exists()) {
            try (InputStream is = context.getAssets().open(assetName);
                 FileOutputStream fos = new FileOutputStream(file)) {
                byte[] buffer = new byte[1024];
                int read;
                while ((read = is.read(buffer)) != -1) {
                    fos.write(buffer, 0, read);
                }
            }
        }
        return file.getAbsolutePath();
    }
}
