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

    /**
     * 将 assets 下的目录（含全部 .dcm 文件）拷贝到外部私有存储，返回目标目录。
     * 已拷贝过（文件数一致）则直接复用，避免重复 IO。
     */
    @JvmStatic
    @Throws(IOException::class)
    suspend fun copyAssetDirToFiles(context: Context, assetDir: String, targetName: String): File =
        withContext(Dispatchers.IO) {
            val names = context.assets.list(assetDir)
                ?: throw IOException("Asset dir not found: $assetDir")
            val dcmNames = names.filter { it.lowercase().endsWith(".dcm") }
            if (dcmNames.isEmpty()) throw IOException("No .dcm files in assets/$assetDir")

            val outDir = File(context.getExternalFilesDir(null), targetName)
            val existing = outDir.listFiles { f -> f.name.endsWith(".dcm", true) }
            if (existing?.size == dcmNames.size) {
                Log.d("FileUtil", "Reusing cached asset dir: ${outDir.absolutePath}")
                return@withContext outDir
            }
            outDir.mkdirs()
            dcmNames.forEach { name ->
                val target = File(outDir, name)
                if (!target.exists()) {
                    copyAsset(context, "$assetDir/$name", target)
                }
            }
            Log.d("FileUtil", "Copied ${dcmNames.size} files to ${outDir.absolutePath}")
            outDir
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
