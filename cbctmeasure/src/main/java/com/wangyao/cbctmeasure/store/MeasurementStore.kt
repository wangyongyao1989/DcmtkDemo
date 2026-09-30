package com.wangyao.cbctmeasure.store

import android.content.Context
import android.util.Log
import com.wangyao.cbctmeasure.jni.AnnotationJni
import com.wangyao.cbctmeasure.jni.MeasureJni
import java.io.File
import java.security.MessageDigest

/**
 * 归档文件的路径约定（PRD 8.4）。
 *
 * 全部集中在这一处，避免"测量存 filesDir、报告存 cacheDir"这类散写；
 * 报告与 SR 落在 getExternalFilesDir()（应用可卸载清理、用户可用文件管理器取走），
 * 测量数据落在 filesDir（不允许被外部改动的结构化数据）。
 */
object StorePaths {

    private const val TAG = "CBCT_MEASURE_STORE"

    const val DIR_MEASUREMENTS = "measurements"
    const val DIR_ANNOTATIONS = "annotations"
    const val DIR_PLANS = "plans"
    const val DIR_SCREENSHOTS = "screenshots"
    const val DIR_REPORTS = "reports"
    const val DIR_SR = "sr"

    fun internal(context: Context, name: String): File =
        File(context.filesDir, name).apply { if (!exists()) mkdirs() }

    /**
     * 外部应用专属目录（PRD 8.4 的报告/截图输出位置）。
     * getExternalFilesDir 在部分机型/存储未挂载时返回 null，此时必须退回到内部存储，
     * 否则"导出报告"会静默失败——这里退回并打日志，让上层至少能提示用户导出位置。
     */
    fun external(context: Context, name: String): File {
        val base = context.getExternalFilesDir(null) ?: context.filesDir
        return File(base, name).apply { if (!exists()) mkdirs() }
    }

    /** PRD 8.4 的命名：{StudyUID}_{SeriesUID}_{kind}.json */
    fun archiveFile(context: Context, kind: String, key: String): File = when (kind) {
        KIND_MEASURE -> File(internal(context, DIR_MEASUREMENTS), "${key}_measure.json")
        KIND_ANNO -> File(internal(context, DIR_ANNOTATIONS), "${key}_anno.json")
        KIND_PLAN -> File(internal(context, DIR_PLANS), "${key}_plan.json")
        else -> File(internal(context, DIR_MEASUREMENTS), "${key}_$kind.json")
    }

    fun screenshotFile(context: Context, studyUid: String, timestamp: Long): File =
        File(internal(context, DIR_SCREENSHOTS), "${studyUid}_$timestamp.png")

    fun reportFile(context: Context, studyUid: String, timestamp: Long): File =
        File(external(context, DIR_REPORTS), "${studyUid}_$timestamp.pdf")

    fun srFile(context: Context, studyUid: String, timestamp: Long): File =
        File(external(context, DIR_SR), "${studyUid}_$timestamp.dcm")

    const val KIND_MEASURE = "measure"
    const val KIND_ANNO = "anno"
    const val KIND_PLAN = "plan"

    /** 列出某归档键下存在的全部文件（界面"历史归档"列表用） */
    fun listArchives(context: Context, key: String): List<File> {
        val out = ArrayList<File>(3)
        for (kind in listOf(KIND_MEASURE, KIND_ANNO, KIND_PLAN)) {
            val f = runCatching { archiveFile(context, kind, key) }.getOrNull() ?: continue
            if (f.exists()) out.add(f)
        }
        Log.d(TAG, "listArchives key=$key found=${out.size}")
        return out
    }
}

/**
 * 测量/标注/方案的本地归档（PRD 5.6 + 8.4，风险项 R-10 的应对）。
 *
 * 三条硬要求：
 *   1) 原子写：先写 .tmp 再 rename，中途被杀不会留下半截 JSON；
 *   2) 校验和：信封里带 payload 的 SHA-1，读出后先校验，不符则回退 .bak；
 *   3) 保留上一版：覆盖前把旧文件复制成 .bak，医生误清一次还能救回。
 *
 * 内容本体一律来自 Native 的 dumpRecords()/dumpPlan()/dumpAnnotations()：
 * 本类不重新组织字段，只做"信封 + 落盘 + 回读"，因此磁盘格式与 JNI 传输格式同源。
 */
object MeasurementStore {

    private const val TAG = "CBCT_MEASURE_STORE"

    private const val ENVELOPE_VERSION = 1
    private const val TMP_SUFFIX = ".tmp"
    private const val BAK_SUFFIX = ".bak"

    /** 一次保存/读取的结果；message 直接给 UI 展示 */
    data class Result(
        val ok: Boolean,
        val file: File,
        val usedBackup: Boolean,
        val message: String,
    ) {
        fun pathText(): String = file.absolutePath
    }

    /** 会话三类数据一起存；任意一类失败都会在 message 里列出，不回滚已成功的部分 */
    fun saveAll(context: Context, handle: Long, key: String): List<Result> {
        if (handle == 0L) return listOf(Result(false, context.filesDir, false, "会话未创建"))
        val out = ArrayList<Result>(3)
        out.add(save(context, StorePaths.KIND_MEASURE, key, MeasureJni.dumpRecords(handle)))
        out.add(save(context, StorePaths.KIND_PLAN, key, MeasureJni.dumpPlan(handle)))
        out.add(save(context, StorePaths.KIND_ANNO, key, AnnotationJni.dumpAnnotations(handle)))
        Log.i(TAG, "saveAll key=$key ok=${out.count { it.ok }}/${out.size}")
        return out
    }

    /** 会话三类数据一起恢复；返回是否至少恢复到一份可用数据 */
    fun loadAll(context: Context, handle: Long, key: String): List<Result> {
        if (handle == 0L) return listOf(Result(false, context.filesDir, false, "会话未创建"))
        val out = ArrayList<Result>(3)

        val m = read(context, StorePaths.KIND_MEASURE, key)
        out.add(if (m.ok) Result(restoreRecordsSafe(handle, m.payload), m.file, m.usedBackup, "测量 ${countOf(m.payload, "records")}")
        else m.asEmpty())

        val p = read(context, StorePaths.KIND_PLAN, key)
        out.add(if (p.ok) Result(restorePlanSafe(handle, p.payload), p.file, p.usedBackup, "方案 ${countOf(p.payload, "implants")}")
        else p.asEmpty())

        val a = read(context, StorePaths.KIND_ANNO, key)
        out.add(if (a.ok) Result(AnnotationJni.loadAnnotations(handle, a.payload), a.file, a.usedBackup, "标注 ${countOf(a.payload, "annotations")}")
        else a.asEmpty())

        Log.i(TAG, "loadAll key=$key ok=${out.count { it.ok }}/${out.size}")
        return out
    }

    /** 存在可恢复归档的键（UI 判断"是否显示恢复按钮"） */
    fun hasArchive(context: Context, key: String): Boolean =
        StorePaths.listArchives(context, key).isNotEmpty()

    // -------------------------------------------------------------------------
    // 单类数据的存/取
    // -------------------------------------------------------------------------

    fun save(context: Context, kind: String, key: String, payload: String): Result {
        val file = StorePaths.archiveFile(context, kind, key)
        val tmp = File(file.parentFile, file.name + TMP_SUFFIX)
        val envelope = buildEnvelope(kind, payload)
        return try {
            file.parentFile?.mkdirs()
            // 覆盖前先给旧主档拍一份 .bak：本次写入一旦落地，上一版就只剩这份备份了
            backupExisting(file)
            tmp.writeText(envelope, Charsets.UTF_8)
            if (!renameOverwriting(tmp, file)) {
                tmp.delete()
                return Result(false, file, false, "写入失败：${file.name}")
            }
            Result(true, file, false, "已保存 ${file.name}")
        } catch (e: Exception) {
            Log.e(TAG, "save kind=$kind failed", e)
            tmp.delete()
            Result(false, file, false, "保存异常：${e.message}")
        }
    }

    /** 读取结果：payload 是剥掉信封的原始 dump 文本 */
    data class ReadResult(
        val ok: Boolean,
        val file: File,
        val usedBackup: Boolean,
        val payload: String,
        val message: String,
    )

    fun read(context: Context, kind: String, key: String): ReadResult {
        val file = StorePaths.archiveFile(context, kind, key)
        val primary = readEnvelope(file)
        if (primary != null) return ReadResult(true, file, false, primary, "主档")
        val bak = File(file.parentFile, file.name + BAK_SUFFIX)
        if (bak.exists()) {
            val fromBak = readEnvelope(bak)
            if (fromBak != null) {
                Log.w(TAG, "主档不可用，已回退备份：${file.name}")
                return ReadResult(true, file, true, fromBak, "备份档")
            }
        }
        return ReadResult(false, file, false, "", if (file.exists()) "校验失败" else "无归档")
    }

    /** 截图落盘（PRD 8.4 的 screenshots 目录；报告证据图同时留档一份） */
    fun saveScreenshot(context: Context, studyUid: String, bitmap: android.graphics.Bitmap): Result {
        val file = StorePaths.screenshotFile(context, studyUid.ifEmpty { "unknownStudy" }, System.currentTimeMillis())
        return try {
            file.parentFile?.mkdirs()
            file.outputStream().use { ok -> bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, ok) }
            Result(file.exists(), file, false, if (file.exists()) "截图 ${file.name}" else "截图写入失败")
        } catch (e: Exception) {
            Log.e(TAG, "saveScreenshot failed", e)
            Result(false, file, false, "截图异常：${e.message}")
        }
    }

    fun clearArchive(context: Context, key: String) {
        for (f in StorePaths.listArchives(context, key)) {
            runCatching { f.delete() }
            runCatching { File(f.parentFile, f.name + BAK_SUFFIX).delete() }
        }
    }

    // -------------------------------------------------------------------------
    // 信封 / 校验
    // -------------------------------------------------------------------------

    /**
     * 信封格式（本类唯一自己组织的 JSON，其余字段透传 Native）：
     * {"envelope":1,"kind":"...","savedAt":...,"checksum":"sha1","payload":"<dump 文本>"}
     * payload 用字符串存放而不是内联对象：这样 Native 改字段完全不需要动归档层，
     * 代价是多一层转义，读回时 unquote 即可。
     */
    private fun buildEnvelope(kind: String, payload: String): String {
        val env = org.json.JSONObject()
        env.put("envelope", ENVELOPE_VERSION)
        env.put("kind", kind)
        env.put("savedAt", System.currentTimeMillis())
        env.put("checksum", sha1(payload))
        env.put("payload", payload)
        return env.toString()
    }

    /** 解析并校验；任何不一致都返回 null（调用方据此决定是否走备份） */
    private fun readEnvelope(file: File): String? {
        if (!file.exists()) return null
        return try {
            val env = org.json.JSONObject(file.readText(Charsets.UTF_8))
            val payload = env.optString("payload")
            if (payload.isEmpty()) return null
            val sum = env.optString("checksum")
            if (sum.isNotEmpty() && !sum.equals(sha1(payload), ignoreCase = true)) {
                Log.w(TAG, "checksum mismatch: ${file.name}")
                return null
            }
            payload
        } catch (e: Exception) {
            Log.w(TAG, "envelope parse failed: ${file.name} ${e.message}")
            null
        }
    }

    private fun sha1(text: String): String {
        val digest = MessageDigest.getInstance("SHA-1").digest(text.toByteArray(Charsets.UTF_8))
        val sb = StringBuilder(digest.size * 2)
        for (b in digest) sb.append(String.format("%02x", b))
        return sb.toString()
    }

    /**
     * 覆盖式 rename。
     * File.renameTo 在目标已存在时跨某些文件系统会失败，先删再移更稳；
     * 但删之前源文件已经落盘，所以中间任何一步崩溃都不会丢数据。
     */
    private fun renameOverwriting(from: File, to: File): Boolean {
        if (to.exists() && !to.delete()) return false
        return from.renameTo(to) || runCatching { to.writeBytes(from.readBytes()).let { from.delete(); true } }.getOrDefault(false)
    }

    /**
     * 覆盖前把现有主档复制成 .bak（PRD R-10：误删/误覆盖一次还能回滚上一版）。
     * 复制失败只降级为"没有备份"，不阻断本次保存。
     */
    private fun backupExisting(target: File) {
        if (!target.exists() || target.length() == 0L) return
        val bak = File(target.parentFile, target.name + BAK_SUFFIX)
        runCatching { target.copyTo(bak, overwrite = true) }
            .onFailure { Log.w(TAG, "backup ${target.name} failed: ${it.message}") }
    }

    private fun restoreRecordsSafe(handle: Long, payload: String): Boolean = try {
        MeasureJni.restoreRecords(handle, payload)
    } catch (e: Exception) {
        Log.e(TAG, "restoreRecords failed", e)
        false
    }

    private fun restorePlanSafe(handle: Long, payload: String): Boolean = try {
        MeasureJni.restorePlan(handle, payload)
    } catch (e: Exception) {
        Log.e(TAG, "restorePlan failed", e)
        false
    }

    private fun countOf(payload: String, field: String): Int = try {
        org.json.JSONObject(payload).optJSONArray(field)?.length() ?: 0
    } catch (e: Exception) {
        0
    }

    private fun ReadResult.asEmpty(): Result = Result(ok, file, usedBackup, message)
}
