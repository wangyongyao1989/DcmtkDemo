package com.example.dcmtk.data

data class DicomWindowInfo(
    val minPixelValue: Int,
    val maxPixelValue: Int,
    val minHu: Int,
    val maxHu: Int,
    val windowCenter: Double,
    val windowWidth: Double
)
