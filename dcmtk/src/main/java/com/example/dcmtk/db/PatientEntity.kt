package com.example.dcmtk.db

import android.content.ContentValues

/**
 * 患者表实体，对应 C# `Model.Patient`。
 *
 * 表名: `patient`
 * 主键: `patient_id`（对应 C# PatientId）
 */
data class PatientEntity(

    /** 患者ID（主键），对应 C# Patient.PatientId */
    val patientId: String,
    /** 患者姓名 */
    val patientName: String,
    /** 身高，C# 固定写 "0" */
    val height: String = "0",
    /** 体重，C# 固定写 "0" */
    val weight: String = "0",
    /** 性别码：M→"1"，其余→"2" */
    val sex: String,
    /** 出生日期，格式 yyyy-MM-dd HH:mm:ss（SQLite TEXT） */
    val birthDate: String,
    /** 出生时间，与出生日期一致 */
    val birthTime: String,
    /** 妊娠状态码：空→"1"，非空→"2" */
    val status: String,
    /** 牙型，C# 固定写 "1" */
    val toothType: String = "1",
    /** 检查状态，C# 固定写 "1" */
    val examineStatus: String = "1",
    /** 检查部位（仅数字与逗号） */
    val bodyPart: String,
    /** 患者年龄 */
    val patientAge: String

) {

    companion object {
        const val TABLE_NAME = "patient"
        const val COL_ID = "patient_id"
        const val COL_NAME = "patient_name"
        const val COL_HEIGHT = "height"
        const val COL_WEIGHT = "weight"
        const val COL_SEX = "sex"
        const val COL_BIRTH_DATE = "birth_date"
        const val COL_BIRTH_TIME = "birth_time"
        const val COL_STATUS = "status"
        const val COL_TOOTH_TYPE = "tooth_type"
        const val COL_EXAMINE_STATUS = "examine_status"
        const val COL_BODY_PART = "body_part"
        const val COL_PATIENT_AGE = "patient_age"

        const val CREATE_TABLE_SQL = """
            CREATE TABLE IF NOT EXISTS $TABLE_NAME (
                $COL_ID            TEXT PRIMARY KEY,
                $COL_NAME          TEXT,
                $COL_HEIGHT        TEXT DEFAULT '0',
                $COL_WEIGHT        TEXT DEFAULT '0',
                $COL_SEX           TEXT,
                $COL_BIRTH_DATE    TEXT,
                $COL_BIRTH_TIME    TEXT,
                $COL_STATUS        TEXT,
                $COL_TOOTH_TYPE    TEXT DEFAULT '1',
                $COL_EXAMINE_STATUS TEXT DEFAULT '1',
                $COL_BODY_PART     TEXT,
                $COL_PATIENT_AGE   TEXT
            )
        """
    }

    fun toContentValues(): ContentValues {
        return ContentValues().apply {
            put(COL_ID, patientId)
            put(COL_NAME, patientName)
            put(COL_HEIGHT, height)
            put(COL_WEIGHT, weight)
            put(COL_SEX, sex)
            put(COL_BIRTH_DATE, birthDate)
            put(COL_BIRTH_TIME, birthTime)
            put(COL_STATUS, status)
            put(COL_TOOTH_TYPE, toothType)
            put(COL_EXAMINE_STATUS, examineStatus)
            put(COL_BODY_PART, bodyPart)
            put(COL_PATIENT_AGE, patientAge)
        }
    }
}
