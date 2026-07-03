package com.example.dcmtk.model

data class PatientRecord(
    val name: String,
    val id: String,
    val sex: String,
    val birthDate: String,
    val accessionNumber: String = "",
    val modality: String = "",
    var isSelected: Boolean = false,
    var isDownloaded: Boolean = false
) {
    override fun toString(): String {
        return name + " (" + id + ")" + (if (accessionNumber.isEmpty()) "" else " Acc:$accessionNumber")
    }
}
