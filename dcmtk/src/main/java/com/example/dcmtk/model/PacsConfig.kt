package com.example.dcmtk.model

data class PacsConfig(
    val host: String,
    val port: Int,
    val localAet: String,
    val remoteAet: String
)
