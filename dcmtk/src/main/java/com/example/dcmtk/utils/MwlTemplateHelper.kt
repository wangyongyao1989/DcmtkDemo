package com.example.dcmtk.utils

import android.content.Context
import android.util.Log
import com.example.dcmtk.PacsManager
import com.example.dcmtk.jni.DcmtkJni
import com.example.dcmtk.model.PacsConfig
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

object MwlTemplateHelper {
    private const val TAG = "MwlTemplateHelper"
    private const val TEMPLATE_ASSETS_DIR = "wlistqry"
    private const val TEMPLATE_LOCAL_DIR = "mwl_templates"
    private const val EXPORT_DIR = "mwl_exports"

    @JvmStatic
    suspend fun prepareTemplates(context: Context) = withContext(Dispatchers.IO) {
        val targetDir = File(context.filesDir, TEMPLATE_LOCAL_DIR)
        if (!targetDir.exists()) {
            targetDir.mkdirs()
        }

        try {
            val files = context.assets.list(TEMPLATE_ASSETS_DIR)
            if (files != null) {
                for (fileName in files) {
                    copyAssetToFile(
                        context, "$TEMPLATE_ASSETS_DIR/$fileName", File(targetDir, fileName)
                    )
                }
            }
        } catch (e: IOException) {
            Log.e(TAG, "Failed to list assets in $TEMPLATE_ASSETS_DIR", e)
        }
    }

    private fun copyAssetToFile(context: Context, assetPath: String, targetFile: File) {
        if (targetFile.exists()) return

        try {
            context.assets.open(assetPath).use { `is` ->
                FileOutputStream(targetFile).use { fos ->
                    val buffer = ByteArray(8192)
                    var read: Int
                    while ((`is`.read(buffer).also { read = it }) != -1) {
                        fos.write(buffer, 0, read)
                    }
                    Log.d(TAG, "Copied template to: ${targetFile.absolutePath}")
                }
            }
        } catch (e: IOException) {
            Log.e(TAG, "Failed to copy asset: $assetPath", e)
        }
    }

    @JvmStatic
    suspend fun executeMwlQuery(
        context: Context,
        config: PacsConfig,
        templateName: String
    ): Array<String> = withContext(Dispatchers.IO) {
        val templateFile = File(context.filesDir, "$TEMPLATE_LOCAL_DIR/$templateName")
        if (!templateFile.exists()) {
            Log.e(TAG, "Template file not found: ${templateFile.absolutePath}")
            return@withContext emptyArray<String>()
        }

        val exportDir = File(context.filesDir, EXPORT_DIR)
        if (!exportDir.exists()) {
            exportDir.mkdirs()
        }

        PacsManager.cFindMWLByTemplate(
            config,
            templateFile.absolutePath, exportDir.absolutePath
        ) ?: emptyArray()
    }

    @JvmStatic
    fun getExportDirPath(context: Context): String {
        val exportDir = File(context.filesDir, EXPORT_DIR)
        if (!exportDir.exists()) {
            exportDir.mkdirs()
        }
        return exportDir.absolutePath
    }
}
