package com.wangyao.cbctdeal.transfer

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException

/**
 * SAF（Storage Access Framework）文件搬运：
 * 解决 Android 10+ 分区存储下 DCMTK 无法直接读取 content://Uri 的问题——
 * 将用户通过 SAF 选择的 CBCT 序列目录整体拷贝到应用私有目录，
 * Native 层从私有目录按文件路径读取。
 */
object CbctFileTransfer {

    /** 私有序列根目录（应用外部私有目录） */
    fun seriesRoot(context: Context): File {
        val dir = File(context.getExternalFilesDir(null), "cbct_series")
        if (!dir.exists()) dir.mkdirs()
        return dir
    }

    /**
     * 将 SAF 选择的目录树拷贝到私有目录。
     * @return 拷贝目标目录
     */
    suspend fun copyTreeUri(context: Context, treeUri: Uri): File =
        withContext(Dispatchers.IO) {
            val root = DocumentFile.fromTreeUri(context, treeUri)
                ?: throw IOException("无法访问所选目录")
            val destRoot = seriesRoot(context)
            val dirName = sanitize(root.name ?: "series_${System.currentTimeMillis()}")
            val dest = File(destRoot, dirName)
            dest.deleteRecursively()
            if (!dest.mkdirs()) throw IOException("创建目录失败: $dest")

            val files = root.listFiles()
            var copied = 0
            for (doc in files) {
                if (!doc.isFile) continue
                val name = doc.name ?: continue
                val out = File(dest, sanitize(name))
                context.contentResolver.openInputStream(doc.uri)?.use { input ->
                    out.outputStream().use { output -> input.copyTo(output) }
                } ?: throw IOException("无法读取文件: $name")
                copied++
            }
            if (copied == 0) {
                dest.deleteRecursively()
                throw IOException("所选目录中没有可解析的文件")
            }
            dest
        }

    /** 过滤文件名中的路径分隔符等非法字符 */
    private fun sanitize(name: String): String =
        name.replace(Regex("[\\\\/:*?\"<>|]"), "_").take(64).ifEmpty { "series" }
}
