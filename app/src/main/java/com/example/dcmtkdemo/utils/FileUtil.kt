package com.example.dcmtkdemo.utils

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

object FileUtil {

    @JvmStatic
    @Throws(IOException::class)
    suspend fun copyAssetToExternalStorage(context: Context, assetName: String, targetName: String): String =
        withContext(Dispatchers.IO) {
            val dir = context.getExternalFilesDir(null) ?: throw IOException("External storage not available")
            val file = File(dir, targetName)
            if (!file.exists()) {
                copyAsset(context, assetName, file)
            }
            file.absolutePath
        }

    @JvmStatic
    @Throws(IOException::class)
    suspend fun copyDcmAssetsToExternal(context: Context) = withContext(Dispatchers.IO) {
        val assets = context.assets.list("")
        assets?.forEach { asset ->
            if (asset.lowercase().endsWith(".dcm")) {
                Log.d("FileUtil", "Copying asset: $asset")
                copyAssetToExternalStorage(context, asset, asset)
            }
        }
    }

    @Throws(IOException::class)
    private fun copyAsset(context: Context, assetName: String, targetFile: File) {
        context.assets.open(assetName).use { `is` ->
            FileOutputStream(targetFile).use { fos ->
                val buffer = ByteArray(1024)
                var read: Int
                while ((`is`.read(buffer).also { read = it }) != -1) {
                    fos.write(buffer, 0, read)
                }
            }
        }
    }
}
