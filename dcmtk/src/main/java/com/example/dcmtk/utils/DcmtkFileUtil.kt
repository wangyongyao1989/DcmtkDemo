package com.example.dcmtk.utils

import android.content.Context
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

object DcmtkFileUtil {
    /**
     * 将 Assets 中的文件拷贝到应用的内部存储目录
     * @param context 上下文
     * @param assetName assets 文件夹中的文件名
     * @return 拷贝后文件的绝对路径
     * @throws IOException 拷贝失败时抛出异常
     */
    @JvmStatic
    @Throws(IOException::class)
    fun copyAssetToInternalStorage(context: Context, assetName: String): String {
        val file = File(context.filesDir, assetName)
        if (!file.exists()) {
            copyAsset(context, assetName, file)
        }
        return file.absolutePath
    }

    @Throws(IOException::class)
    private fun copyAsset(context: Context, assetName: String, targetFile: File) {
        context.assets.open(assetName).use { `is` ->
            FileOutputStream(targetFile).use { fos ->
                val buffer = ByteArray(8192)
                var read: Int
                while ((`is`.read(buffer).also { read = it }) != -1) {
                    fos.write(buffer, 0, read)
                }
            }
        }
    }
}
