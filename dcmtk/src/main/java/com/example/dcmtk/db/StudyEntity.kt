package com.example.dcmtk.db

import android.content.ContentValues

/**
 * 检查表实体，对应 C# `Model.Study`。
 *
 * 表名: `study`
 * 主键: `id`（自增整数，新增时取 max(id)+1）
 */
data class StudyEntity(

    /** 主键ID，新增时取 max(id)+1；修改时从已存在记录获取 */
    val id: String,
    /** 患者ID（外键） */
    val patientId: String,
    /** 标签，C# 固定写 "" */
    val tag: String = "",
    /** 检查号 */
    val accessionNumber: String,
    /** 检查ID（由 AccessionNumber 数字部分生成，前缀 "CR"） */
    val studyId: String,
    /** 检查实例UID */
    val studyInstanceId: String,
    /** 提交日期，格式 yyyy-MM-dd HH:mm:ss */
    val submitDate: String,
    /** 模态码：CR/空→"1"，其余→"2" */
    val modalities: String,
    /** 检查部位 */
    val bodyPart: String,
    /** 检查描述 */
    val studyDescription: String,
    /** 检查描述补充，C# 固定写 "" */
    val examineDescription: String = "",
    /** 患者年龄 */
    val patientAge: String,
    /** 申请医师ID（对应 C# ExamineDoctorID，来自 ReferringPhysicianName） */
    val examineDoctorId: String,
    /** 审核医师ID（对应 C# ReviewDoctorID，来自 PerformingPhysicianName） */
    val reviewDoctorId: String,
    /** 状态：新增时 "1" */
    val status: String,
    /** DCM文件路径，C# 固定写 "" */
    val dcmPath: String = "",
    /** SmaType：新增时 "1" */
    val smaType: String = "1"

) {

    companion object {
        const val TABLE_NAME = "study"
        const val COL_ID = "id"
        const val COL_PATIENT_ID = "patient_id"
        const val COL_TAG = "tag"
        const val COL_ACCESSION_NUMBER = "accession_number"
        const val COL_STUDY_ID = "study_id"
        const val COL_STUDY_INSTANCE_ID = "study_instance_id"
        const val COL_SUBMIT_DATE = "submit_date"
        const val COL_MODALITIES = "modalities"
        const val COL_BODY_PART = "body_part"
        const val COL_STUDY_DESCRIPTION = "study_description"
        const val COL_EXAMINE_DESCRIPTION = "examine_description"
        const val COL_PATIENT_AGE = "patient_age"
        const val COL_EXAMINE_DOCTOR_ID = "examine_doctor_id"
        const val COL_REVIEW_DOCTOR_ID = "review_doctor_id"
        const val COL_STATUS = "status"
        const val COL_DCM_PATH = "dcm_path"
        const val COL_SMA_TYPE = "sma_type"

        const val CREATE_TABLE_SQL = """
            CREATE TABLE IF NOT EXISTS $TABLE_NAME (
                $COL_ID                 INTEGER PRIMARY KEY,
                $COL_PATIENT_ID         TEXT,
                $COL_TAG                TEXT DEFAULT '',
                $COL_ACCESSION_NUMBER   TEXT,
                $COL_STUDY_ID           TEXT,
                $COL_STUDY_INSTANCE_ID  TEXT,
                $COL_SUBMIT_DATE        TEXT,
                $COL_MODALITIES         TEXT,
                $COL_BODY_PART          TEXT,
                $COL_STUDY_DESCRIPTION  TEXT,
                $COL_EXAMINE_DESCRIPTION TEXT DEFAULT '',
                $COL_PATIENT_AGE        TEXT,
                $COL_EXAMINE_DOCTOR_ID  TEXT,
                $COL_REVIEW_DOCTOR_ID   TEXT,
                $COL_STATUS             TEXT,
                $COL_DCM_PATH           TEXT DEFAULT '',
                $COL_SMA_TYPE           TEXT DEFAULT '1'
            )
        """

        /** 在 study_instance_id 上建索引，加速存在性查询 */
        const val CREATE_INDEX_STUDY_UID_SQL = """
            CREATE INDEX IF NOT EXISTS idx_study_instance_id
            ON $TABLE_NAME ($COL_STUDY_INSTANCE_ID)
        """

        /** 在 (patient_id, accession_number) 上建联合索引 */
        const val CREATE_INDEX_ACC_SQL = """
            CREATE INDEX IF NOT EXISTS idx_patient_acc
            ON $TABLE_NAME ($COL_PATIENT_ID, $COL_ACCESSION_NUMBER)
        """
    }

    fun toContentValues(): ContentValues {
        return ContentValues().apply {
            put(COL_ID, id)
            put(COL_PATIENT_ID, patientId)
            put(COL_TAG, tag)
            put(COL_ACCESSION_NUMBER, accessionNumber)
            put(COL_STUDY_ID, studyId)
            put(COL_STUDY_INSTANCE_ID, studyInstanceId)
            put(COL_SUBMIT_DATE, submitDate)
            put(COL_MODALITIES, modalities)
            put(COL_BODY_PART, bodyPart)
            put(COL_STUDY_DESCRIPTION, studyDescription)
            put(COL_EXAMINE_DESCRIPTION, examineDescription)
            put(COL_PATIENT_AGE, patientAge)
            put(COL_EXAMINE_DOCTOR_ID, examineDoctorId)
            put(COL_REVIEW_DOCTOR_ID, reviewDoctorId)
            put(COL_STATUS, status)
            put(COL_DCM_PATH, dcmPath)
            put(COL_SMA_TYPE, smaType)
        }
    }
}
