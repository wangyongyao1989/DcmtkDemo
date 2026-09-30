package com.wangyao.cbctmeasure.ai

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/**
 * 内置牙科 CBCT 数据的交付（PRD 5.6 AI-01 的验证数据，随 :cbctmeasure 出包）。
 *
 * 数据不是随便挑的：全部来自公开 **DentVoxel** 牙科 CBCT 数据集
 * （专家逐牙标注），主机侧用 DICOM 写入器转成本工程解析器直接吃的
 * "192x192x128 @0.60mm 符号 int16 序列"，因此：
 *   - 它是真实临床采集的体数据，不是合成数组；
 *   - 每个病例在 assets 里都带一份 expert GT（掩膜）与来源说明，
 *     主机脚本可以拿同一份 GT 计算 Dice，与真机上的分割结果口径一致。
 *
 * 为什么要内置而不是让用户自己找 DICOM：AI 的验收（AC-08 主机/真机奇偶校验）
 * 要求"输入完全相同"。病例固定 = 特征张量固定 = 概率图固定，
 * 真机 dump 出来的 feat/prob 才能与主机 fixture 逐元素比对。
 *
 * 释放路径与 CT Preprocess / neck_ct 同一套（getExternalFilesDir），
 * 解析器只认普通目录里的 .dcm 文件。
 */
object DentalDataAssets {

    private const val TAG = "CbctMeasureAi"

    /** assets 下的根目录名（与模块 README 里的数据章节一一对应） */
    const val ASSET_ROOT = "dental_cbct"

    /** 一个可选病例 */
    data class Case(
        val id: String,             // 目录名，如 dentvoxel_0101
        val title: String,          // UI 显示名
        val note: String,           // 来源 / 是否参与训练 / GT 情况
        val assetDir: String,       // assets 内路径
        val sliceCount: Int,        // .dcm 数量（0 = 目录里没有，UI 置灰）
    )

    /** 列出 assets 里的病例；index.json 缺失时退化为"扫目录" */
    fun listCases(ctx: Context): List<Case> {
        val out = ArrayList<Case>()
        val index = try {
            ctx.assets.open("$ASSET_ROOT/index.json").use { it.readBytes().toString(Charsets.UTF_8) }
        } catch (e: IOException) {
            null
        }
        if (index != null) {
            try {
                val arr = JSONObject(index).optJSONArray("cases")
                if (arr != null) {
                    for (i in 0 until arr.length()) {
                        val o = arr.getJSONObject(i)
                        val dir = o.optString("assetDir")
                        if (dir.isEmpty()) continue
                        out.add(
                            Case(
                                id = o.optString("id", dir.substringAfterLast('/')),
                                title = o.optString("title", dir),
                                note = o.optString("note"),
                                assetDir = dir,
                                sliceCount = countDcm(ctx, dir),
                            )
                        )
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "index.json 解析失败: ${e.message}")
            }
        }
        if (out.isEmpty()) {
            // 兜底：没有索引也至少把"能选的目录"给出来，别让按钮空转
            val names = try {
                ctx.assets.list(ASSET_ROOT)?.toList() ?: emptyList()
            } catch (e: IOException) {
                emptyList()
            }
            names.filter { !it.endsWith(".json") }.forEach { name ->
                val dir = "$ASSET_ROOT/$name"
                out.add(Case(name, name, "内置牙科 CBCT", dir, countDcm(ctx, dir)))
            }
        }
        return out.sortedBy { it.id }
    }

    private fun countDcm(ctx: Context, dir: String): Int = try {
        ctx.assets.list(dir)?.count { it.lowercase().endsWith(".dcm") } ?: 0
    } catch (e: IOException) {
        0
    }

    /**
     * 把病例释放到普通可读目录，返回该目录（已释放过则直接复用）。
     *
     * 复用判据是"目录里的 .dcm 标记文件"而不是文件数：拷贝过程可能被系统杀掉，
     * 留下半截 DICOM 文件；只看数量会让解析器读到一张坏文件并给出难懂的错。
     * 标记里记着病例 id 与张数，任何一项不符就整批重拷。
     */
    suspend fun release(ctx: Context, case: Case): File = withContext(Dispatchers.IO) {
        if (case.sliceCount <= 0) throw IOException("assets 里找不到 ${case.assetDir} 的 .dcm")
        // getExternalFilesDir 在外部存储不可用时返回 null；此时若继续拼路径会得到
        // 一个相对目录，解析器读到的是"看不见的空目录"，报错还指着病例名，很难查
        val base = ctx.getExternalFilesDir(null)
            ?: throw IOException("应用外部私有目录不可用（getExternalFilesDir 返回 null）")
        val names = ctx.assets.list(case.assetDir)
            ?.filter { it.lowercase().endsWith(".dcm") }
            ?.sorted() ?: throw IOException("assets.list 失败: ${case.assetDir}")
        val outDir = File(base, "dental_${case.id}")
        if (markerMatches(outDir, case.id, names.size)) {
            Log.d(TAG, "复用已释放的病例 ${outDir.absolutePath}")
            return@withContext outDir
        }
        outDir.mkdirs()
        File(outDir, MARKER).delete()      // 拷贝开始前先作废，防止"半批"被当成完整
        names.forEach { name ->
            ctx.assets.open("${case.assetDir}/$name").use { input ->
                FileOutputStream(File(outDir, name)).use { input.copyTo(it, 16 * 1024) }
            }
        }
        val landed = outDir.listFiles { f ->
            f.isFile && f.name.lowercase().endsWith(".dcm") && f.length() > 0
        }?.size ?: 0
        if (landed != names.size) throw IOException("释放后只有 $landed/${names.size} 张有效 DICOM")
        File(outDir, MARKER).writeText("$case.id ${names.size}\n")
        Log.d(TAG, "病例 ${case.id} 已释放 ${names.size} 张 -> ${outDir.absolutePath}")
        outDir
    }

    private const val MARKER = ".release_ok"

    private fun markerMatches(dir: File, id: String, count: Int): Boolean {
        val m = File(dir, MARKER)
        if (!m.isFile) return false
        val text = try {
            m.readText().trim()
        } catch (e: IOException) {
            return false
        }
        return text == "$id $count"
    }

    // 注：expert GT（.nii.gz）只在主机侧参与 Dice 计算，不随 APK 释放，
    // 也不参与解析；它留在 assets 里作为"这个病例是什么、标注从哪来"的证据。
}
