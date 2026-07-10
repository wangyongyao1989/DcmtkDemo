package com.example.dcmtk.db

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.util.Log
import com.example.dcmtk.model.WorklistItem
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * MWL 同步仓库 —— 核心实现 C# mwl_query 中的"查询-匹配-存在则修改-不存在则插入"逻辑。
 *
 * 流程对照 C# FindWorklistItems:
 * 1. 遍历每条 WorklistItem
 * 2. 模态过滤（Code 49 开启时仅保留 CR/DR）
 * 3. 患者 upsert：按 PatientID 查询 → 存在则 UPDATE，不存在则 INSERT
 * 4. 检查 upsert：
 *    a. 按 StudyInstanceUID 查询 → isStudyUidExist
 *    b. 按 PatientID + AccessionNumber 查询 → isAccessionNumberExist
 *    c. 按 Code 47/48 开关组合判断 isStudyExist
 *    d. 存在则 UPDATE（取匹配到的记录 ID），不存在则 INSERT
 * 5. 全程在单事务中执行，保证原子性
 */
class MwlSyncRepository private constructor(
    private val dbHelper: MwlDatabase
) {

    private val dbDateFmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)

    /**
     * 同步结果统计。
     */
    data class SyncResult(
        val totalItems: Int,
        val skippedByModality: Int,
        val patientInserted: Int,
        val patientUpdated: Int,
        val studyInserted: Int,
        val studyUpdated: Int,
        val errors: List<String>
    ) {
        override fun toString(): String {
            return "SyncResult(total=$totalItems, skipped=$skippedByModality, " +
                    "patient[ins=$patientInserted, upd=$patientUpdated], " +
                    "study[ins=$studyInserted, upd=$studyUpdated], errors=${errors.size})"
        }
    }

    /**
     * 批量同步 MWL 查询结果到数据库。
     *
     * @param items  MWL C-FIND 返回并映射后的 WorklistItem 列表
     * @param config 同步配置开关
     * @return 同步结果统计
     */
    fun syncWorklistItems(items: List<WorklistItem>, config: MwlSyncConfig): SyncResult {
        val errors = mutableListOf<String>()
        var skipped = 0
        var patIns = 0
        var patUpd = 0
        var stdIns = 0
        var stdUpd = 0

        val db = dbHelper.writableDatabase
        db.beginTransaction()
        try {
            val (studyUidFlag, accFlag) = config.resolveStudyCheckFlags()
            Log.i(TAG, "Sync start: ${items.size} items, " +
                    "studyUidFlag=$studyUidFlag, accFlag=$accFlag, " +
                    "modalityFilter=${config.checkModalityEnable}")

            for ((index, item) in items.withIndex()) {
                try {
                    // --- 步骤1: 模态过滤 ---
                    if (config.checkModalityEnable && !item.isModalityAllowed()) {
                        Log.d(TAG, "[$index] Skipped by modality filter: ${item.modality}")
                        skipped++
                        continue
                    }

                    // --- 步骤2: 患者 upsert ---
                    val effectivePatientId = item.patientID.ifEmpty {
                        generateAutoPatientId()
                    }

                    val patientExisted = if (effectivePatientId.isNotEmpty()) {
                        patientExists(db, effectivePatientId)
                    } else false

                    if (patientExisted) {
                        updatePatient(db, item, effectivePatientId)
                        patUpd++
                        Log.d(TAG, "[$index] Patient updated: $effectivePatientId")
                    } else {
                        insertPatient(db, item, effectivePatientId)
                        patIns++
                        Log.d(TAG, "[$index] Patient inserted: $effectivePatientId")
                    }

                    // --- 步骤3: 检查 upsert ---
                    val (isStudyExist, matchedStudyId) = checkStudyExistence(
                        db, item, studyUidFlag, accFlag
                    )

                    if (isStudyExist && matchedStudyId != null) {
                        updateStudy(db, item, effectivePatientId, matchedStudyId, config)
                        stdUpd++
                        Log.d(TAG, "[$index] Study updated: id=$matchedStudyId, uid=${item.studyInstanceUID}")
                    } else {
                        val newStudyId = generateStudyId(db, item.accessionNumber, index)
                        insertStudy(db, item, effectivePatientId, newStudyId, config)
                        stdIns++
                        Log.d(TAG, "[$index] Study inserted: studyId=$newStudyId, uid=${item.studyInstanceUID}")
                    }

                } catch (e: Exception) {
                    val msg = "[$index] Error syncing patientID=${item.patientID}, " +
                            "studyUID=${item.studyInstanceUID}: ${e.message}"
                    Log.e(TAG, msg, e)
                    errors.add(msg)
                }
            }

            db.setTransactionSuccessful()
            Log.i(TAG, "Sync done: patIns=$patIns, patUpd=$patUpd, stdIns=$stdIns, stdUpd=$stdUpd, skipped=$skipped")
        } finally {
            db.endTransaction()
        }

        return SyncResult(
            totalItems = items.size,
            skippedByModality = skipped,
            patientInserted = patIns,
            patientUpdated = patUpd,
            studyInserted = stdIns,
            studyUpdated = stdUpd,
            errors = errors
        )
    }

    // ===================== 患者表操作 =====================

    /**
     * 按 PatientID 查询患者是否存在。
     * 对应 C# `new ModelView.Patient().Exists(PatientID)`
     */
    private fun patientExists(db: SQLiteDatabase, patientId: String): Boolean {
        val cursor = db.query(
            PatientEntity.TABLE_NAME,
            arrayOf(PatientEntity.COL_ID),
            "${PatientEntity.COL_ID} = ?",
            arrayOf(patientId),
            null, null, null, "1"
        )
        return cursor.use { it.count > 0 }
    }

    /**
     * 插入新患者。
     * 对应 C# `new ModelView.Patient().Add(patient)`
     */
    private fun insertPatient(
        db: SQLiteDatabase, item: WorklistItem, patientId: String
    ) {
        val birthDateStr = formatDicomDate(item.patientBirthDate)
        val entity = PatientEntity(
            patientId = patientId,
            patientName = item.patientName,
            sex = item.resolveSexCode(),
            birthDate = birthDateStr,
            birthTime = birthDateStr,
            status = item.resolvePregnancyStatusCode(),
            bodyPart = item.resolveBodyPart(),
            patientAge = item.resolveAge()
        )
        db.insert(PatientEntity.TABLE_NAME, null, entity.toContentValues())
    }

    /**
     * 更新已存在患者。
     * 对应 C# `new ModelView.Patient().Update(patient)`
     */
    private fun updatePatient(
        db: SQLiteDatabase, item: WorklistItem, patientId: String
    ) {
        val birthDateStr = formatDicomDate(item.patientBirthDate)
        val cv = ContentValues().apply {
            put(PatientEntity.COL_NAME, item.patientName)
            put(PatientEntity.COL_SEX, item.resolveSexCode())
            put(PatientEntity.COL_BIRTH_DATE, birthDateStr)
            put(PatientEntity.COL_BIRTH_TIME, birthDateStr)
            put(PatientEntity.COL_STATUS, item.resolvePregnancyStatusCode())
            put(PatientEntity.COL_BODY_PART, item.resolveBodyPart())
            put(PatientEntity.COL_PATIENT_AGE, item.resolveAge())
        }
        db.update(
            PatientEntity.TABLE_NAME, cv,
            "${PatientEntity.COL_ID} = ?", arrayOf(patientId)
        )
    }

    // ===================== 检查表操作 =====================

    /**
     * 检查存在性判断 —— 核心逻辑。
     *
     * 对应 C# 中以下代码块:
     * ```csharp
     * DataTable ds = ...GetList("studyInstanceID = '" + StudyInstanceUID + "'");
     * bool isStudyUidExist = StudyInstanceUID == "" ? false : ds.Rows.Count > 0;
     * DataTable ds0 = ...GetList("patientID = '" + PatientID + "' AND AccessionNumber = '" + AccessionNumber + "'");
     * bool isAccessionNumberExist = AccessionNumber == "" ? false : ds0.Rows.Count > 0;
     *
     * if (bCheckAccessionNumberEnable) {
     *     if (bCheckStudyUidEnable) {
     *         isStudyExist = isStudyUidExist || isAccessionNumberExist;
     *     } else {
     *         isStudyExist = isAccessionNumberExist;
     *     }
     *     if (ds.Rows.Count < 1 && ds0.Rows.Count > 0 && isStudyExist) {
     *         ds = ds0;
     *     }
     * } else {
     *     isStudyExist = isStudyUidExist;
     * }
     * ```
     *
     * @return (isStudyExist, matchedStudyId) — matchedStudyId 为匹配到的记录主键
     */
    private fun checkStudyExistence(
        db: SQLiteDatabase,
        item: WorklistItem,
        studyUidFlag: Boolean,
        accFlag: Boolean
    ): Pair<Boolean, String?> {
        // a. 按 StudyInstanceUID 查询
        var studyUidMatchedId: String? = null
        val isStudyUidExist = if (item.studyInstanceUID.isNotEmpty()) {
            studyUidMatchedId = queryStudyIdByUid(db, item.studyInstanceUID)
            studyUidMatchedId != null
        } else false

        // b. 按 PatientID + AccessionNumber 查询
        var accMatchedId: String? = null
        val isAccessionNumberExist = if (item.accessionNumber.isNotEmpty() && item.patientID.isNotEmpty()) {
            accMatchedId = queryStudyIdByPatientAndAcc(db, item.patientID, item.accessionNumber)
            accMatchedId != null
        } else false

        // c. 按开关组合判断
        val isStudyExist: Boolean
        var matchedId: String? = null

        if (accFlag) {
            isStudyExist = if (studyUidFlag) {
                isStudyUidExist || isAccessionNumberExist
            } else {
                isAccessionNumberExist
            }
            // C# 回退: UID 查不到但 AccessionNumber 查到了且判断为存在 → 用 AccessionNumber 的结果
            matchedId = when {
                studyUidMatchedId != null -> studyUidMatchedId
                accMatchedId != null && isStudyExist -> accMatchedId
                else -> null
            }
        } else {
            isStudyExist = isStudyUidExist
            matchedId = studyUidMatchedId
        }

        Log.d(TAG, "checkStudyExistence: uidExist=$isStudyUidExist(uidId=$studyUidMatchedId), " +
                "accExist=$isAccessionNumberExist(accId=$accMatchedId), " +
                "result=$isStudyExist(matchedId=$matchedId)")

        return Pair(isStudyExist, matchedId)
    }

    /**
     * 按 StudyInstanceUID 查询检查记录主键。
     */
    private fun queryStudyIdByUid(db: SQLiteDatabase, studyUid: String): String? {
        return querySingleStudyId(
            db,
            "${StudyEntity.COL_STUDY_INSTANCE_ID} = ?",
            arrayOf(studyUid)
        )
    }

    /**
     * 按 PatientID + AccessionNumber 查询检查记录主键。
     */
    private fun queryStudyIdByPatientAndAcc(
        db: SQLiteDatabase, patientId: String, accessionNumber: String
    ): String? {
        return querySingleStudyId(
            db,
            "${StudyEntity.COL_PATIENT_ID} = ? AND ${StudyEntity.COL_ACCESSION_NUMBER} = ?",
            arrayOf(patientId, accessionNumber)
        )
    }

    private fun querySingleStudyId(
        db: SQLiteDatabase, selection: String, args: Array<String>
    ): String? {
        val cursor = db.query(
            StudyEntity.TABLE_NAME,
            arrayOf(StudyEntity.COL_ID),
            selection, args, null, null, null, "1"
        )
        return cursor.use {
            if (it.moveToFirst()) it.getString(0) else null
        }
    }

    /**
     * 插入新检查。
     * 对应 C# `new ModelView.Study().Add(study)`
     */
    private fun insertStudy(
        db: SQLiteDatabase,
        item: WorklistItem,
        patientId: String,
        studyId: String,
        config: MwlSyncConfig
    ) {
        val id = generateNextStudyDbId(db)
        val entity = StudyEntity(
            id = id,
            patientId = patientId,
            accessionNumber = item.accessionNumber,
            studyId = studyId,
            studyInstanceId = item.studyInstanceUID,
            submitDate = dbDateFmt.format(Date()),
            modalities = item.resolveModalityCode(),
            bodyPart = item.resolveBodyPart(),
            studyDescription = item.studyDescription,
            patientAge = item.resolveAge(),
            examineDoctorId = item.referringPhysicianName.ifEmpty { config.fallbackUserId },
            reviewDoctorId = item.performingPhysicianName.ifEmpty { config.fallbackUserId },
            status = "1",
            smaType = "1"
        )
        db.insert(StudyEntity.TABLE_NAME, null, entity.toContentValues())
    }

    /**
     * 更新已存在检查。
     * 对应 C# `new ModelView.Study().Update(study)`
     */
    private fun updateStudy(
        db: SQLiteDatabase,
        item: WorklistItem,
        patientId: String,
        existingId: String,
        config: MwlSyncConfig
    ) {
        val cv = ContentValues().apply {
            put(StudyEntity.COL_PATIENT_ID, patientId)
            put(StudyEntity.COL_ACCESSION_NUMBER, item.accessionNumber)
            put(StudyEntity.COL_STUDY_INSTANCE_ID, item.studyInstanceUID)
            put(StudyEntity.COL_SUBMIT_DATE, dbDateFmt.format(Date()))
            put(StudyEntity.COL_MODALITIES, item.resolveModalityCode())
            put(StudyEntity.COL_BODY_PART, item.resolveBodyPart())
            put(StudyEntity.COL_STUDY_DESCRIPTION, item.studyDescription)
            put(StudyEntity.COL_PATIENT_AGE, item.resolveAge())
            put(StudyEntity.COL_EXAMINE_DOCTOR_ID, item.referringPhysicianName.ifEmpty { config.fallbackUserId })
            put(StudyEntity.COL_REVIEW_DOCTOR_ID, item.performingPhysicianName.ifEmpty { config.fallbackUserId })
        }
        db.update(
            StudyEntity.TABLE_NAME, cv,
            "${StudyEntity.COL_ID} = ?", arrayOf(existingId)
        )
    }

    // ===================== ID 生成 =====================

    /**
     * 生成下一个检查记录主键（max(id)+1）。
     * 对应 C# `new ModelView.Study().GetMaxId() + ""`
     */
    private fun generateNextStudyDbId(db: SQLiteDatabase): String {
        val cursor = db.rawQuery(
            "SELECT IFNULL(MAX(${StudyEntity.COL_ID}), 0) + 1 FROM ${StudyEntity.TABLE_NAME}",
            null
        )
        return cursor.use {
            if (it.moveToFirst()) it.getInt(0).toString() else "1"
        }
    }

    /**
     * 生成 StudyID —— 完整复刻 C# 中 AccessionNumber → StudyID 的转换逻辑。
     *
     * C# 逻辑:
     * 1. 提取 AccessionNumber 中的数字部分
     * 2. 若数字长度 < 1: 由时间戳生成 + 序号后缀
     * 3. 若数字长度 < 15: "CR" + 数字
     * 4. 若数字长度 >= 15: 取末 14 位 + "CR" 前缀
     */
    private fun generateStudyId(db: SQLiteDatabase, accessionNumber: String, index: Int): String {
        val digits = accessionNumber.replace(Regex("[^0-9]+"), "")

        if (digits.length < 1) {
            val timeBased = generateTimeBasedStudyId(index)
            return "CR$timeBased"
        }

        return if (digits.length < 15) {
            "CR$digits"
        } else {
            val tail = digits.takeLast(14)
            "CR$tail"
        }
    }

    /**
     * 时间戳 + 序号生成 StudyID 后缀，替代 C# AutoGenerateIdController。
     */
    private fun generateTimeBasedStudyId(index: Int): String {
        val timestamp = System.currentTimeMillis()
        val timeStr = timestamp.toString()
        val tail12 = if (timeStr.length >= 12) timeStr.takeLast(12) else timeStr.padStart(12, '0')
        val indexPart = if (index < 10) "0$index" else index.toString()
        return tail12 + indexPart
    }

    /**
     * 自动生成患者 ID，替代 C# AutoGenerateIdController.GetAutoGeneratePatientId()。
     * 格式: "P" + yyyyMMddHHmmssSSS
     */
    private fun generateAutoPatientId(): String {
        val fmt = SimpleDateFormat("yyyyMMddHHmmssSSS", Locale.US)
        return "P${fmt.format(Date())}"
    }

    // ===================== 工具方法 =====================

    /**
     * 将 DICOM 日期(yyyyMMdd)转为数据库 TEXT 格式(yyyy-MM-dd HH:mm:ss)。
     * 对应 C# `DateTime.ParseExact(PatientBirthDate, "yyyyMMdd", ...)` 后存库。
     */
    private fun formatDicomDate(dicomDate: String): String {
        if (dicomDate.isEmpty()) return dbDateFmt.format(Date())
        return try {
            val srcFmt = SimpleDateFormat("yyyyMMdd", Locale.US)
            val date = srcFmt.parse(dicomDate) ?: return dbDateFmt.format(Date())
            dbDateFmt.format(date)
        } catch (e: Exception) {
            dbDateFmt.format(Date())
        }
    }

    companion object {
        private const val TAG = "MwlSyncRepository"

        @Volatile
        private var instance: MwlSyncRepository? = null

        /**
         * 获取单例。
         */
        fun getInstance(context: Context): MwlSyncRepository {
            return instance ?: synchronized(this) {
                instance ?: MwlSyncRepository(
                    MwlDatabase.getInstance(context)
                ).also { instance = it }
            }
        }

        /**
         * 便捷入口：接收 [com.example.dcmtk.PacsManager.cFindMWL] 的原始返回值，
         * 自动完成 映射 → 同步 全流程。
         *
         * @param context  Android Context
         * @param rawResults  cFindMWL 返回的 HashMap 数组（可能为 null）
         * @param config  同步配置
         * @return 同步结果统计
         */
        fun syncFromRawResults(
            context: Context,
            rawResults: Array<HashMap<String, String>>?,
            config: MwlSyncConfig
        ): SyncResult {
            if (rawResults.isNullOrEmpty()) {
                Log.w(TAG, "syncFromRawResults: no results to sync")
                return SyncResult(0, 0, 0, 0, 0, 0, emptyList())
            }
            val items = com.example.dcmtk.model.WorklistItemMapper.fromMapList(rawResults)
            return getInstance(context).syncWorklistItems(items, config)
        }
    }
}
