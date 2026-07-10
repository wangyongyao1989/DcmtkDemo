package com.example.dcmtk.db

/**
 * MWL 同步配置开关，对应 C# CodeMaster 中的配置项：
 *
 * - Code 47 → [checkStudyInstanceUIDEnable]：是否按 StudyInstanceUID 判断检查已存在
 * - Code 48 → [checkAccessionNumberEnable]：是否按 PatientID + AccessionNumber 判断检查已存在
 * - Code 49 → [checkModalityEnable]：是否按模态过滤（仅同步 CR/DR）
 *
 * C# 中的回退逻辑：当 47 和 48 均关闭时，强制将 47 置为 true（至少要有一个判断依据）。
 * 该回退逻辑由 [MwlSyncRepository] 在执行时处理，此处的值保持原始配置。
 *
 * @param checkStudyInstanceUIDEnable 对应 Code 47
 * @param checkAccessionNumberEnable  对应 Code 48
 * @param checkModalityEnable         对应 Code 49
 * @param fallbackUserId              医师字段为空时的回退用户 ID（对应 C# LoginWinClass.UserId）
 */
data class MwlSyncConfig(
    val checkStudyInstanceUIDEnable: Boolean = false,
    val checkAccessionNumberEnable: Boolean = false,
    val checkModalityEnable: Boolean = false,
    val fallbackUserId: String = "admin"
) {

    /**
     * 应用 C# 的回退规则：当 StudyUID 和 AccessionNumber 开关都关闭时，
     * 强制以 StudyUID 作为判断依据。
     */
    fun resolveStudyCheckFlags(): Pair<Boolean, Boolean> {
        var studyUidFlag = checkStudyInstanceUIDEnable
        val accFlag = checkAccessionNumberEnable
        if (!studyUidFlag && !accFlag) {
            studyUidFlag = true
        }
        return Pair(studyUidFlag, accFlag)
    }
}
