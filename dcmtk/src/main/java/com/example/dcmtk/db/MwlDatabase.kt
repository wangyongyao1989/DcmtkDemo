package com.example.dcmtk.db

import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper

/**
 * MWL 同步数据库，基于 SQLiteOpenHelper。
 *
 * 存储 MWL C-FIND 查询结果同步落库的患者表 [PatientEntity] 与检查表 [StudyEntity]。
 * 采用 SQLite 而非 Room，避免引入 KSP 插件，零额外依赖即可编译。
 *
 * 数据库文件位于应用私有目录: `context.databaseDir/mwl_sync.db`
 */
class MwlDatabase private constructor(
    context: Context
) : SQLiteOpenHelper(
    context.applicationContext,
    DB_NAME,
    null,
    DB_VERSION
) {

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(PatientEntity.CREATE_TABLE_SQL)
        db.execSQL(StudyEntity.CREATE_TABLE_SQL)
        db.execSQL(StudyEntity.CREATE_INDEX_STUDY_UID_SQL)
        db.execSQL(StudyEntity.CREATE_INDEX_ACC_SQL)
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
        db.execSQL("DROP TABLE IF EXISTS ${StudyEntity.TABLE_NAME}")
        db.execSQL("DROP TABLE IF EXISTS ${PatientEntity.TABLE_NAME}")
        onCreate(db)
    }

    companion object {
        private const val DB_NAME = "mwl_sync.db"
        private const val DB_VERSION = 1

        @Volatile
        private var instance: MwlDatabase? = null

        /**
         * 获取单例实例。内部使用 ApplicationContext，避免 Activity 泄漏。
         */
        fun getInstance(context: Context): MwlDatabase {
            return instance ?: synchronized(this) {
                instance ?: MwlDatabase(context).also { instance = it }
            }
        }
    }
}
