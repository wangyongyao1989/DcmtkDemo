package com.example.dcmtk.utils

/**
 * DICOM Tag Information mapping from drtstrct.h
 */
data class DicomTagInfo(
    val group: Int,
    val element: Int,
    val name: String,
    val vr: String,
    val vm: String,
    val type: String,
    val module: String
) {
    val tag: Int get() = (group shl 16) or element
    val formattedTag: String get() = String.format("(%04X,%04X)", group, element).lowercase()

    override fun toString(): String {
        return String.format(
            "(%04X,%04X) %s [%s] VM=%s Module=%s",
            group,
            element,
            name,
            vr,
            vm,
            module
        )
    }
}

object DicomTag {
    // --- PatientModule (M) ---
    val PatientName = DicomTagInfo(0x0010, 0x0010, "PatientName", "PN", "1", "2", "PatientModule")
    val PatientID = DicomTagInfo(0x0010, 0x0020, "PatientID", "LO", "1", "2", "PatientModule")
    val IssuerOfPatientID =
        DicomTagInfo(0x0010, 0x0021, "IssuerOfPatientID", "LO", "1", "3", "PatientModule")
    val IssuerOfPatientIDQualifiersSequence = DicomTagInfo(
        0x0010,
        0x0024,
        "IssuerOfPatientIDQualifiersSequence",
        "SQ",
        "1",
        "3",
        "PatientModule"
    )
    val TypeOfPatientID =
        DicomTagInfo(0x0010, 0x0022, "TypeOfPatientID", "CS", "1", "3", "PatientModule")
    val PatientBirthDate =
        DicomTagInfo(0x0010, 0x0030, "PatientBirthDate", "DA", "1", "2", "PatientModule")
    val PatientBirthDateInAlternativeCalendar = DicomTagInfo(
        0x0010,
        0x0033,
        "PatientBirthDateInAlternativeCalendar",
        "LO",
        "1",
        "3",
        "PatientModule"
    )
    val PatientDeathDateInAlternativeCalendar = DicomTagInfo(
        0x0010,
        0x0034,
        "PatientDeathDateInAlternativeCalendar",
        "LO",
        "1",
        "3",
        "PatientModule"
    )
    val PatientAlternativeCalendar =
        DicomTagInfo(0x0010, 0x0035, "PatientAlternativeCalendar", "CS", "1", "1C", "PatientModule")
    val PatientSex = DicomTagInfo(0x0010, 0x0040, "PatientSex", "CS", "1", "2", "PatientModule")
    val ReferencedPatientPhotoSequence = DicomTagInfo(
        0x0010,
        0x1100,
        "ReferencedPatientPhotoSequence",
        "SQ",
        "1",
        "3",
        "PatientModule"
    )
    val QualityControlSubject =
        DicomTagInfo(0x0010, 0x0200, "QualityControlSubject", "CS", "1", "3", "PatientModule")
    val ReferencedPatientSequence =
        DicomTagInfo(0x0008, 0x1120, "ReferencedPatientSequence", "SQ", "1", "3", "PatientModule")
    val PatientBirthTime =
        DicomTagInfo(0x0010, 0x0032, "PatientBirthTime", "TM", "1", "3", "PatientModule")
    val OtherPatientIDsSequence =
        DicomTagInfo(0x0010, 0x1002, "OtherPatientIDsSequence", "SQ", "1", "3", "PatientModule")
    val OtherPatientNames =
        DicomTagInfo(0x0010, 0x1001, "OtherPatientNames", "PN", "1-n", "3", "PatientModule")
    val EthnicGroup = DicomTagInfo(0x0010, 0x2160, "EthnicGroup", "SH", "1", "3", "PatientModule")
    val PatientComments =
        DicomTagInfo(0x0010, 0x4000, "PatientComments", "LT", "1", "3", "PatientModule")
    val PatientSpeciesDescription =
        DicomTagInfo(0x0010, 0x2201, "PatientSpeciesDescription", "LO", "1", "1C", "PatientModule")
    val PatientSpeciesCodeSequence =
        DicomTagInfo(0x0010, 0x2202, "PatientSpeciesCodeSequence", "SQ", "1", "1C", "PatientModule")
    val PatientBreedDescription =
        DicomTagInfo(0x0010, 0x2292, "PatientBreedDescription", "LO", "1", "2C", "PatientModule")
    val PatientBreedCodeSequence =
        DicomTagInfo(0x0010, 0x2293, "PatientBreedCodeSequence", "SQ", "1", "2C", "PatientModule")
    val BreedRegistrationSequence =
        DicomTagInfo(0x0010, 0x2294, "BreedRegistrationSequence", "SQ", "1", "2C", "PatientModule")
    val StrainDescription =
        DicomTagInfo(0x0010, 0x0212, "StrainDescription", "UC", "1", "3", "PatientModule")
    val StrainNomenclature =
        DicomTagInfo(0x0010, 0x0213, "StrainNomenclature", "LO", "1", "3", "PatientModule")
    val StrainCodeSequence =
        DicomTagInfo(0x0010, 0x0219, "StrainCodeSequence", "SQ", "1", "3", "PatientModule")
    val StrainAdditionalInformation =
        DicomTagInfo(0x0010, 0x0218, "StrainAdditionalInformation", "UT", "1", "3", "PatientModule")
    val StrainStockSequence =
        DicomTagInfo(0x0010, 0x0216, "StrainStockSequence", "SQ", "1", "3", "PatientModule")
    val GeneticModificationsSequence = DicomTagInfo(
        0x0010,
        0x0221,
        "GeneticModificationsSequence",
        "SQ",
        "1",
        "3",
        "PatientModule"
    )
    val ResponsiblePerson =
        DicomTagInfo(0x0010, 0x2297, "ResponsiblePerson", "PN", "1", "2C", "PatientModule")
    val ResponsiblePersonRole =
        DicomTagInfo(0x0010, 0x2298, "ResponsiblePersonRole", "CS", "1", "1C", "PatientModule")
    val ResponsibleOrganization =
        DicomTagInfo(0x0010, 0x2299, "ResponsibleOrganization", "LO", "1", "2C", "PatientModule")
    val PatientIdentityRemoved =
        DicomTagInfo(0x0012, 0x0062, "PatientIdentityRemoved", "CS", "1", "3", "PatientModule")
    val DeidentificationMethod =
        DicomTagInfo(0x0012, 0x0063, "DeidentificationMethod", "LO", "1-n", "1C", "PatientModule")
    val DeidentificationMethodCodeSequence = DicomTagInfo(
        0x0012,
        0x0064,
        "DeidentificationMethodCodeSequence",
        "SQ",
        "1",
        "1C",
        "PatientModule"
    )
    val SourcePatientGroupIdentificationSequence = DicomTagInfo(
        0x0010,
        0x0026,
        "SourcePatientGroupIdentificationSequence",
        "SQ",
        "1",
        "3",
        "PatientModule"
    )
    val GroupOfPatientsIdentificationSequence = DicomTagInfo(
        0x0010,
        0x0027,
        "GroupOfPatientsIdentificationSequence",
        "SQ",
        "1",
        "3",
        "PatientModule"
    )

    // --- ClinicalTrialSubjectModule (U) ---
    val ClinicalTrialSponsorName = DicomTagInfo(
        0x0012,
        0x0010,
        "ClinicalTrialSponsorName",
        "LO",
        "1",
        "1",
        "ClinicalTrialSubjectModule"
    )
    val ClinicalTrialProtocolID = DicomTagInfo(
        0x0012,
        0x0020,
        "ClinicalTrialProtocolID",
        "LO",
        "1",
        "1",
        "ClinicalTrialSubjectModule"
    )
    val ClinicalTrialProtocolName = DicomTagInfo(
        0x0012,
        0x0021,
        "ClinicalTrialProtocolName",
        "LO",
        "1",
        "2",
        "ClinicalTrialSubjectModule"
    )
    val ClinicalTrialSiteID = DicomTagInfo(
        0x0012,
        0x0030,
        "ClinicalTrialSiteID",
        "LO",
        "1",
        "2",
        "ClinicalTrialSubjectModule"
    )
    val ClinicalTrialSiteName = DicomTagInfo(
        0x0012,
        0x0031,
        "ClinicalTrialSiteName",
        "LO",
        "1",
        "2",
        "ClinicalTrialSubjectModule"
    )
    val ClinicalTrialSubjectID = DicomTagInfo(
        0x0012,
        0x0040,
        "ClinicalTrialSubjectID",
        "LO",
        "1",
        "1C",
        "ClinicalTrialSubjectModule"
    )
    val ClinicalTrialSubjectReadingID = DicomTagInfo(
        0x0012,
        0x0042,
        "ClinicalTrialSubjectReadingID",
        "LO",
        "1",
        "1C",
        "ClinicalTrialSubjectModule"
    )
    val ClinicalTrialProtocolEthicsCommitteeName = DicomTagInfo(
        0x0012,
        0x0081,
        "ClinicalTrialProtocolEthicsCommitteeName",
        "LO",
        "1",
        "1C",
        "ClinicalTrialSubjectModule"
    )
    val ClinicalTrialProtocolEthicsCommitteeApprovalNumber = DicomTagInfo(
        0x0012,
        0x0082,
        "ClinicalTrialProtocolEthicsCommitteeApprovalNumber",
        "LO",
        "1",
        "3",
        "ClinicalTrialSubjectModule"
    )

    // --- GeneralStudyModule (M) ---
    val StudyInstanceUID =
        DicomTagInfo(0x0020, 0x000D, "StudyInstanceUID", "UI", "1", "1", "GeneralStudyModule")
    val StudyDate = DicomTagInfo(0x0008, 0x0020, "StudyDate", "DA", "1", "2", "GeneralStudyModule")
    val StudyTime = DicomTagInfo(0x0008, 0x0030, "StudyTime", "TM", "1", "2", "GeneralStudyModule")
    val ReferringPhysicianName =
        DicomTagInfo(0x0008, 0x0090, "ReferringPhysicianName", "PN", "1", "2", "GeneralStudyModule")
    val ReferringPhysicianIdentificationSequence = DicomTagInfo(
        0x0008,
        0x0096,
        "ReferringPhysicianIdentificationSequence",
        "SQ",
        "1",
        "3",
        "GeneralStudyModule"
    )
    val ConsultingPhysicianName = DicomTagInfo(
        0x0008,
        0x009C,
        "ConsultingPhysicianName",
        "PN",
        "1-n",
        "3",
        "GeneralStudyModule"
    )
    val ConsultingPhysicianIdentificationSequence = DicomTagInfo(
        0x0008,
        0x009D,
        "ConsultingPhysicianIdentificationSequence",
        "SQ",
        "1",
        "3",
        "GeneralStudyModule"
    )
    val StudyID = DicomTagInfo(0x0020, 0x0010, "StudyID", "SH", "1", "2", "GeneralStudyModule")
    val AccessionNumber =
        DicomTagInfo(0x0008, 0x0050, "AccessionNumber", "SH", "1", "2", "GeneralStudyModule")
    val IssuerOfAccessionNumberSequence = DicomTagInfo(
        0x0008,
        0x0051,
        "IssuerOfAccessionNumberSequence",
        "SQ",
        "1",
        "3",
        "GeneralStudyModule"
    )
    val StudyDescription =
        DicomTagInfo(0x0008, 0x1030, "StudyDescription", "LO", "1", "3", "GeneralStudyModule")
    val PerformingPhysicianName =
        DicomTagInfo(0x0008, 0x1050, "PerformingPhysicianName", "PN", "1", "3", "GeneralStudyModule")
    val PhysiciansOfRecord =
        DicomTagInfo(0x0008, 0x1048, "PhysiciansOfRecord", "PN", "1-n", "3", "GeneralStudyModule")
    val PhysiciansOfRecordIdentificationSequence = DicomTagInfo(
        0x0008,
        0x1049,
        "PhysiciansOfRecordIdentificationSequence",
        "SQ",
        "1",
        "3",
        "GeneralStudyModule"
    )
    val NameOfPhysiciansReadingStudy = DicomTagInfo(
        0x0008,
        0x1060,
        "NameOfPhysiciansReadingStudy",
        "PN",
        "1-n",
        "3",
        "GeneralStudyModule"
    )
    val PhysiciansReadingStudyIdentificationSequence = DicomTagInfo(
        0x0008,
        0x1062,
        "PhysiciansReadingStudyIdentificationSequence",
        "SQ",
        "1",
        "3",
        "GeneralStudyModule"
    )
    val RequestingService =
        DicomTagInfo(0x0032, 0x1033, "RequestingService", "LO", "1", "3", "GeneralStudyModule")
    val RequestingServiceCodeSequence = DicomTagInfo(
        0x0032,
        0x1034,
        "RequestingServiceCodeSequence",
        "SQ",
        "1",
        "3",
        "GeneralStudyModule"
    )
    val ReferencedStudySequence = DicomTagInfo(
        0x0008,
        0x1110,
        "ReferencedStudySequence",
        "SQ",
        "1",
        "3",
        "GeneralStudyModule"
    )
    val ProcedureCodeSequence =
        DicomTagInfo(0x0008, 0x1032, "ProcedureCodeSequence", "SQ", "1", "3", "GeneralStudyModule")
    val ReasonForPerformedProcedureCodeSequence = DicomTagInfo(
        0x0040,
        0x1012,
        "ReasonForPerformedProcedureCodeSequence",
        "SQ",
        "1",
        "3",
        "GeneralStudyModule"
    )

    // --- PatientStudyModule (U) ---
    val AdmittingDiagnosesDescription = DicomTagInfo(
        0x0008,
        0x1080,
        "AdmittingDiagnosesDescription",
        "LO",
        "1-n",
        "3",
        "PatientStudyModule"
    )
    val AdmittingDiagnosesCodeSequence = DicomTagInfo(
        0x0008,
        0x1084,
        "AdmittingDiagnosesCodeSequence",
        "SQ",
        "1",
        "3",
        "PatientStudyModule"
    )
    val PatientAge =
        DicomTagInfo(0x0010, 0x1010, "PatientAge", "AS", "1", "3", "PatientStudyModule")
    val PatientSize =
        DicomTagInfo(0x0010, 0x1020, "PatientSize", "DS", "1", "3", "PatientStudyModule")
    val PatientWeight =
        DicomTagInfo(0x0010, 0x1030, "PatientWeight", "DS", "1", "3", "PatientStudyModule")
    val PatientBodyMassIndex =
        DicomTagInfo(0x0010, 0x1022, "PatientBodyMassIndex", "DS", "1", "3", "PatientStudyModule")
    val MeasuredAPDimension =
        DicomTagInfo(0x0010, 0x1023, "MeasuredAPDimension", "DS", "1", "3", "PatientStudyModule")
    val MeasuredLateralDimension = DicomTagInfo(
        0x0010,
        0x1024,
        "MeasuredLateralDimension",
        "DS",
        "1",
        "3",
        "PatientStudyModule"
    )
    val PatientSizeCodeSequence = DicomTagInfo(
        0x0010,
        0x1021,
        "PatientSizeCodeSequence",
        "SQ",
        "1",
        "3",
        "PatientStudyModule"
    )
    val MedicalAlerts =
        DicomTagInfo(0x0010, 0x2000, "MedicalAlerts", "LO", "1-n", "3", "PatientStudyModule")
    val Allergies =
        DicomTagInfo(0x0010, 0x2110, "Allergies", "LO", "1-n", "3", "PatientStudyModule")
    val SmokingStatus =
        DicomTagInfo(0x0010, 0x21A0, "SmokingStatus", "CS", "1", "3", "PatientStudyModule")
    val PregnancyStatus =
        DicomTagInfo(0x0010, 0x21C0, "PregnancyStatus", "US", "1", "3", "PatientStudyModule")
    val LastMenstrualDate =
        DicomTagInfo(0x0010, 0x21D0, "LastMenstrualDate", "DA", "1", "3", "PatientStudyModule")
    val PatientState =
        DicomTagInfo(0x0038, 0x0500, "PatientState", "LO", "1", "3", "PatientStudyModule")
    val Occupation =
        DicomTagInfo(0x0010, 0x2180, "Occupation", "SH", "1", "3", "PatientStudyModule")
    val AdditionalPatientHistory = DicomTagInfo(
        0x0010,
        0x21B0,
        "AdditionalPatientHistory",
        "LT",
        "1",
        "3",
        "PatientStudyModule"
    )
    val AdmissionID =
        DicomTagInfo(0x0038, 0x0010, "AdmissionID", "LO", "1", "3", "PatientStudyModule")
    val IssuerOfAdmissionIDSequence = DicomTagInfo(
        0x0038,
        0x0014,
        "IssuerOfAdmissionIDSequence",
        "SQ",
        "1",
        "3",
        "PatientStudyModule"
    )
    val ReasonForVisit =
        DicomTagInfo(0x0032, 0x1066, "ReasonForVisit", "UT", "1", "3", "PatientStudyModule")
    val ReasonForVisitCodeSequence = DicomTagInfo(
        0x0032,
        0x1067,
        "ReasonForVisitCodeSequence",
        "SQ",
        "1",
        "3",
        "PatientStudyModule"
    )
    val ServiceEpisodeID =
        DicomTagInfo(0x0038, 0x0060, "ServiceEpisodeID", "LO", "1", "3", "PatientStudyModule")
    val IssuerOfServiceEpisodeIDSequence = DicomTagInfo(
        0x0038,
        0x0064,
        "IssuerOfServiceEpisodeIDSequence",
        "SQ",
        "1",
        "3",
        "PatientStudyModule"
    )
    val ServiceEpisodeDescription = DicomTagInfo(
        0x0038,
        0x0062,
        "ServiceEpisodeDescription",
        "LO",
        "1",
        "3",
        "PatientStudyModule"
    )
    val PatientSexNeutered =
        DicomTagInfo(0x0010, 0x2203, "PatientSexNeutered", "CS", "1", "2C", "PatientStudyModule")

    // --- ClinicalTrialStudyModule (U) ---
    val ClinicalTrialTimePointID = DicomTagInfo(
        0x0012,
        0x0050,
        "ClinicalTrialTimePointID",
        "LO",
        "1",
        "2",
        "ClinicalTrialStudyModule"
    )
    val ClinicalTrialTimePointDescription = DicomTagInfo(
        0x0012,
        0x0051,
        "ClinicalTrialTimePointDescription",
        "ST",
        "1",
        "3",
        "ClinicalTrialStudyModule"
    )
    val ClinicalTrialTimePointTypeCodeSequence = DicomTagInfo(
        0x0012,
        0x0054,
        "ClinicalTrialTimePointTypeCodeSequence",
        "SQ",
        "1",
        "3",
        "ClinicalTrialStudyModule"
    )
    val LongitudinalTemporalOffsetFromEvent = DicomTagInfo(
        0x0012,
        0x0052,
        "LongitudinalTemporalOffsetFromEvent",
        "FD",
        "1",
        "3",
        "ClinicalTrialStudyModule"
    )
    val LongitudinalTemporalEventType = DicomTagInfo(
        0x0012,
        0x0053,
        "LongitudinalTemporalEventType",
        "CS",
        "1",
        "1C",
        "ClinicalTrialStudyModule"
    )
    val ConsentForClinicalTrialUseSequence = DicomTagInfo(
        0x0012,
        0x0083,
        "ConsentForClinicalTrialUseSequence",
        "SQ",
        "1",
        "3",
        "ClinicalTrialStudyModule"
    )

    // --- GeneralSeriesModule (M) ---
    val BodyPartExamined =
        DicomTagInfo(0x0018, 0x0015, "BodyPartExamined", "CS", "1", "2", "GeneralSeriesModule")

    // --- RTSeriesModule (M) ---
    val Modality = DicomTagInfo(0x0008, 0x0060, "Modality", "CS", "1", "1", "RTSeriesModule")
    val SeriesInstanceUID =
        DicomTagInfo(0x0020, 0x000E, "SeriesInstanceUID", "UI", "1", "1", "RTSeriesModule")
    val SeriesNumber =
        DicomTagInfo(0x0020, 0x0011, "SeriesNumber", "IS", "1", "2", "RTSeriesModule")
    val SeriesDate = DicomTagInfo(0x0008, 0x0021, "SeriesDate", "DA", "1", "3", "RTSeriesModule")
    val SeriesTime = DicomTagInfo(0x0008, 0x0031, "SeriesTime", "TM", "1", "3", "RTSeriesModule")
    val SeriesDescription =
        DicomTagInfo(0x0008, 0x103E, "SeriesDescription", "LO", "1", "3", "RTSeriesModule")
    val SeriesDescriptionCodeSequence = DicomTagInfo(
        0x0008,
        0x103F,
        "SeriesDescriptionCodeSequence",
        "SQ",
        "1",
        "3",
        "RTSeriesModule"
    )
    val OperatorsName =
        DicomTagInfo(0x0008, 0x1070, "OperatorsName", "PN", "1-n", "2", "RTSeriesModule")
    val OperatorIdentificationSequence = DicomTagInfo(
        0x0008,
        0x1072,
        "OperatorIdentificationSequence",
        "SQ",
        "1",
        "3",
        "RTSeriesModule"
    )
    val ReferencedPerformedProcedureStepSequence = DicomTagInfo(
        0x0008,
        0x1111,
        "ReferencedPerformedProcedureStepSequence",
        "SQ",
        "1",
        "3",
        "RTSeriesModule"
    )
    val RequestAttributesSequence =
        DicomTagInfo(0x0040, 0x0275, "RequestAttributesSequence", "SQ", "1", "3", "RTSeriesModule")
    val PerformedProcedureStepID =
        DicomTagInfo(0x0040, 0x0253, "PerformedProcedureStepID", "SH", "1", "3", "RTSeriesModule")
    val PerformedProcedureStepStartDate = DicomTagInfo(
        0x0040,
        0x0244,
        "PerformedProcedureStepStartDate",
        "DA",
        "1",
        "3",
        "RTSeriesModule"
    )
    val PerformedProcedureStepStartTime = DicomTagInfo(
        0x0040,
        0x0245,
        "PerformedProcedureStepStartTime",
        "TM",
        "1",
        "3",
        "RTSeriesModule"
    )
    val PerformedProcedureStepEndDate = DicomTagInfo(
        0x0040,
        0x0250,
        "PerformedProcedureStepEndDate",
        "DA",
        "1",
        "3",
        "RTSeriesModule"
    )
    val PerformedProcedureStepEndTime = DicomTagInfo(
        0x0040,
        0x0251,
        "PerformedProcedureStepEndTime",
        "TM",
        "1",
        "3",
        "RTSeriesModule"
    )
    val PerformedProcedureStepDescription = DicomTagInfo(
        0x0040,
        0x0254,
        "PerformedProcedureStepDescription",
        "LO",
        "1",
        "3",
        "RTSeriesModule"
    )
    val PerformedProtocolCodeSequence = DicomTagInfo(
        0x0040,
        0x0260,
        "PerformedProtocolCodeSequence",
        "SQ",
        "1",
        "3",
        "RTSeriesModule"
    )
    val CommentsOnThePerformedProcedureStep = DicomTagInfo(
        0x0040,
        0x0280,
        "CommentsOnThePerformedProcedureStep",
        "ST",
        "1",
        "3",
        "RTSeriesModule"
    )
    val TreatmentSessionUID =
        DicomTagInfo(0x300A, 0x0700, "TreatmentSessionUID", "UI", "1", "3", "RTSeriesModule")

    // --- ClinicalTrialSeriesModule (U) ---
    val ClinicalTrialCoordinatingCenterName = DicomTagInfo(
        0x0012,
        0x0060,
        "ClinicalTrialCoordinatingCenterName",
        "LO",
        "1",
        "2",
        "ClinicalTrialSeriesModule"
    )
    val ClinicalTrialSeriesID = DicomTagInfo(
        0x0012,
        0x0071,
        "ClinicalTrialSeriesID",
        "LO",
        "1",
        "3",
        "ClinicalTrialSeriesModule"
    )
    val ClinicalTrialSeriesDescription = DicomTagInfo(
        0x0012,
        0x0072,
        "ClinicalTrialSeriesDescription",
        "LO",
        "1",
        "3",
        "ClinicalTrialSeriesModule"
    )

    // --- GeneralEquipmentModule (M) ---
    val Manufacturer =
        DicomTagInfo(0x0008, 0x0070, "Manufacturer", "LO", "1", "2", "GeneralEquipmentModule")
    val InstitutionName =
        DicomTagInfo(0x0008, 0x0080, "InstitutionName", "LO", "1", "3", "GeneralEquipmentModule")
    val InstitutionAddress =
        DicomTagInfo(0x0008, 0x0081, "InstitutionAddress", "ST", "1", "3", "GeneralEquipmentModule")
    val StationName =
        DicomTagInfo(0x0008, 0x1010, "StationName", "SH", "1", "3", "GeneralEquipmentModule")
    val InstitutionalDepartmentName = DicomTagInfo(
        0x0008,
        0x1040,
        "InstitutionalDepartmentName",
        "LO",
        "1",
        "3",
        "GeneralEquipmentModule"
    )
    val InstitutionalDepartmentTypeCodeSequence = DicomTagInfo(
        0x0008,
        0x1041,
        "InstitutionalDepartmentTypeCodeSequence",
        "SQ",
        "1",
        "3",
        "GeneralEquipmentModule"
    )
    val ManufacturerModelName = DicomTagInfo(
        0x0008,
        0x1090,
        "ManufacturerModelName",
        "LO",
        "1",
        "3",
        "GeneralEquipmentModule"
    )
    val ManufacturerDeviceClassUID = DicomTagInfo(
        0x0018,
        0x100B,
        "ManufacturerDeviceClassUID",
        "UI",
        "1-n",
        "3",
        "GeneralEquipmentModule"
    )
    val DeviceSerialNumber =
        DicomTagInfo(0x0018, 0x1000, "DeviceSerialNumber", "LO", "1", "3", "GeneralEquipmentModule")
    val SoftwareVersions =
        DicomTagInfo(0x0018, 0x1020, "SoftwareVersions", "LO", "1-n", "3", "GeneralEquipmentModule")
    val GantryID =
        DicomTagInfo(0x0018, 0x1008, "GantryID", "LO", "1", "3", "GeneralEquipmentModule")
    val UDISequence =
        DicomTagInfo(0x0018, 0x100A, "UDISequence", "SQ", "1", "3", "GeneralEquipmentModule")
    val DeviceUID =
        DicomTagInfo(0x0018, 0x1002, "DeviceUID", "UI", "1", "3", "GeneralEquipmentModule")
    val SpatialResolution =
        DicomTagInfo(0x0018, 0x1050, "SpatialResolution", "DS", "1", "3", "GeneralEquipmentModule")
    val DateOfLastCalibration = DicomTagInfo(
        0x0018,
        0x1200,
        "DateOfLastCalibration",
        "DA",
        "1-n",
        "3",
        "GeneralEquipmentModule"
    )
    val TimeOfLastCalibration = DicomTagInfo(
        0x0018,
        0x1201,
        "TimeOfLastCalibration",
        "TM",
        "1-n",
        "3",
        "GeneralEquipmentModule"
    )
    val PixelPaddingValue = DicomTagInfo(
        0x0028,
        0x0120,
        "PixelPaddingValue",
        "US/SS",
        "1",
        "1C",
        "GeneralEquipmentModule"
    )

    // --- FrameOfReferenceModule (U) ---
    val FrameOfReferenceUID = DicomTagInfo(
        0x0020,
        0x0052,
        "FrameOfReferenceUID",
        "UI",
        "1",
        "1",
        "FrameOfReferenceModule"
    )
    val PositionReferenceIndicator = DicomTagInfo(
        0x0020,
        0x1040,
        "PositionReferenceIndicator",
        "LO",
        "1",
        "2",
        "FrameOfReferenceModule"
    )

    // --- StructureSetModule (M) ---
    val StructureSetLabel =
        DicomTagInfo(0x3006, 0x0002, "StructureSetLabel", "SH", "1", "1", "StructureSetModule")
    val StructureSetName =
        DicomTagInfo(0x3006, 0x0004, "StructureSetName", "LO", "1", "3", "StructureSetModule")
    val StructureSetDescription = DicomTagInfo(
        0x3006,
        0x0006,
        "StructureSetDescription",
        "ST",
        "1",
        "3",
        "StructureSetModule"
    )
    val InstanceNumber =
        DicomTagInfo(0x0020, 0x0013, "InstanceNumber", "IS", "1", "3", "StructureSetModule")
    val StructureSetDate =
        DicomTagInfo(0x3006, 0x0008, "StructureSetDate", "DA", "1", "2", "StructureSetModule")
    val StructureSetTime =
        DicomTagInfo(0x3006, 0x0009, "StructureSetTime", "TM", "1", "2", "StructureSetModule")
    val ReferencedFrameOfReferenceSequence = DicomTagInfo(
        0x3006,
        0x0010,
        "ReferencedFrameOfReferenceSequence",
        "SQ",
        "1",
        "3",
        "StructureSetModule"
    )
    val StructureSetROISequence = DicomTagInfo(
        0x3006,
        0x0020,
        "StructureSetROISequence",
        "SQ",
        "1",
        "1",
        "StructureSetModule"
    )
    val PredecessorStructureSetSequence = DicomTagInfo(
        0x3006,
        0x0018,
        "PredecessorStructureSetSequence",
        "SQ",
        "1",
        "3",
        "StructureSetModule"
    )

    // --- ROIContourModule (M) ---
    val ROIContourSequence =
        DicomTagInfo(0x3006, 0x0039, "ROIContourSequence", "SQ", "1", "1", "ROIContourModule")

    // --- RTROIObservationsModule (M) ---
    val RTROIObservationsSequence = DicomTagInfo(
        0x3006,
        0x0080,
        "RTROIObservationsSequence",
        "SQ",
        "1",
        "1",
        "RTROIObservationsModule"
    )

    // --- ApprovalModule (U) ---
    val ApprovalStatus =
        DicomTagInfo(0x300E, 0x0002, "ApprovalStatus", "CS", "1", "1", "ApprovalModule")
    val ReviewDate = DicomTagInfo(0x300E, 0x0004, "ReviewDate", "DA", "1", "2C", "ApprovalModule")
    val ReviewTime = DicomTagInfo(0x300E, 0x0005, "ReviewTime", "TM", "1", "2C", "ApprovalModule")
    val ReviewerName =
        DicomTagInfo(0x300E, 0x0008, "ReviewerName", "PN", "1", "2C", "ApprovalModule")

    // --- GeneralReferenceModule (U) ---
    val ReferencedImageSequence = DicomTagInfo(
        0x0008,
        0x1140,
        "ReferencedImageSequence",
        "SQ",
        "1",
        "3",
        "GeneralReferenceModule"
    )
    val ReferencedInstanceSequence = DicomTagInfo(
        0x0008,
        0x114A,
        "ReferencedInstanceSequence",
        "SQ",
        "1",
        "3",
        "GeneralReferenceModule"
    )
    val DerivationDescription = DicomTagInfo(
        0x0008,
        0x2111,
        "DerivationDescription",
        "ST",
        "1",
        "3",
        "GeneralReferenceModule"
    )
    val DerivationCodeSequence = DicomTagInfo(
        0x0008,
        0x9215,
        "DerivationCodeSequence",
        "SQ",
        "1",
        "3",
        "GeneralReferenceModule"
    )
    val SourceImageSequence = DicomTagInfo(
        0x0008,
        0x2112,
        "SourceImageSequence",
        "SQ",
        "1",
        "3",
        "GeneralReferenceModule"
    )
    val SourceInstanceSequence = DicomTagInfo(
        0x0042,
        0x0013,
        "SourceInstanceSequence",
        "SQ",
        "1",
        "3",
        "GeneralReferenceModule"
    )

    // --- SOPCommonModule (M) ---
    val SOPClassUID = DicomTagInfo(0x0008, 0x0016, "SOPClassUID", "UI", "1", "1", "SOPCommonModule")
    val SOPInstanceUID =
        DicomTagInfo(0x0008, 0x0018, "SOPInstanceUID", "UI", "1", "1", "SOPCommonModule")
    val SpecificCharacterSet =
        DicomTagInfo(0x0008, 0x0005, "SpecificCharacterSet", "CS", "1-n", "1C", "SOPCommonModule")
    val InstanceCreationDate =
        DicomTagInfo(0x0008, 0x0012, "InstanceCreationDate", "DA", "1", "3", "SOPCommonModule")
    val InstanceCreationTime =
        DicomTagInfo(0x0008, 0x0013, "InstanceCreationTime", "TM", "1", "3", "SOPCommonModule")
    val InstanceCoercionDateTime =
        DicomTagInfo(0x0008, 0x0015, "InstanceCoercionDateTime", "DT", "1", "3", "SOPCommonModule")
    val InstanceCreatorUID =
        DicomTagInfo(0x0008, 0x0014, "InstanceCreatorUID", "UI", "1", "3", "SOPCommonModule")
    val RelatedGeneralSOPClassUID = DicomTagInfo(
        0x0008,
        0x001A,
        "RelatedGeneralSOPClassUID",
        "UI",
        "1-n",
        "3",
        "SOPCommonModule"
    )
    val OriginalSpecializedSOPClassUID = DicomTagInfo(
        0x0008,
        0x001B,
        "OriginalSpecializedSOPClassUID",
        "UI",
        "1",
        "3",
        "SOPCommonModule"
    )
    val CodingSchemeIdentificationSequence = DicomTagInfo(
        0x0008,
        0x0110,
        "CodingSchemeIdentificationSequence",
        "SQ",
        "1",
        "3",
        "SOPCommonModule"
    )
    val ContextGroupIdentificationSequence = DicomTagInfo(
        0x0008,
        0x0123,
        "ContextGroupIdentificationSequence",
        "SQ",
        "1",
        "3",
        "SOPCommonModule"
    )
    val MappingResourceIdentificationSequence = DicomTagInfo(
        0x0008,
        0x0124,
        "MappingResourceIdentificationSequence",
        "SQ",
        "1",
        "3",
        "SOPCommonModule"
    )
    val TimezoneOffsetFromUTC =
        DicomTagInfo(0x0008, 0x0201, "TimezoneOffsetFromUTC", "SH", "1", "3", "SOPCommonModule")
    val ContributingEquipmentSequence = DicomTagInfo(
        0x0018,
        0xA001,
        "ContributingEquipmentSequence",
        "SQ",
        "1",
        "3",
        "SOPCommonModule"
    )
    val SOPInstanceStatus =
        DicomTagInfo(0x0100, 0x0410, "SOPInstanceStatus", "CS", "1", "3", "SOPCommonModule")
    val SOPAuthorizationDateTime =
        DicomTagInfo(0x0100, 0x0420, "SOPAuthorizationDateTime", "DT", "1", "3", "SOPCommonModule")
    val SOPAuthorizationComment =
        DicomTagInfo(0x0100, 0x0424, "SOPAuthorizationComment", "LT", "1", "3", "SOPCommonModule")
    val AuthorizationEquipmentCertificationNumber = DicomTagInfo(
        0x0100,
        0x0426,
        "AuthorizationEquipmentCertificationNumber",
        "LO",
        "1",
        "3",
        "SOPCommonModule"
    )
    val MACParametersSequence =
        DicomTagInfo(0x4FFE, 0x0001, "MACParametersSequence", "SQ", "1", "3", "SOPCommonModule")
    val DigitalSignaturesSequence =
        DicomTagInfo(0xFFFA, 0xFFFA, "DigitalSignaturesSequence", "SQ", "1", "3", "SOPCommonModule")
    val EncryptedAttributesSequence = DicomTagInfo(
        0x0400,
        0x0500,
        "EncryptedAttributesSequence",
        "SQ",
        "1",
        "1C",
        "SOPCommonModule"
    )
    val OriginalAttributesSequence = DicomTagInfo(
        0x0400,
        0x0561,
        "OriginalAttributesSequence",
        "SQ",
        "1",
        "3",
        "SOPCommonModule"
    )
    val HL7StructuredDocumentReferenceSequence = DicomTagInfo(
        0x0040,
        0xA390,
        "HL7StructuredDocumentReferenceSequence",
        "SQ",
        "1",
        "1C",
        "SOPCommonModule"
    )
    val LongitudinalTemporalInformationModified = DicomTagInfo(
        0x0028,
        0x0303,
        "LongitudinalTemporalInformationModified",
        "CS",
        "1",
        "3",
        "SOPCommonModule"
    )
    val QueryRetrieveView =
        DicomTagInfo(0x0008, 0x0053, "QueryRetrieveView", "CS", "1", "1C", "SOPCommonModule")
    val ConversionSourceAttributesSequence = DicomTagInfo(
        0x0020,
        0x9172,
        "ConversionSourceAttributesSequence",
        "SQ",
        "1",
        "1C",
        "SOPCommonModule"
    )
    val ContentQualification =
        DicomTagInfo(0x0018, 0x9004, "ContentQualification", "CS", "1", "3", "SOPCommonModule")
    val PrivateDataElementCharacteristicsSequence = DicomTagInfo(
        0x0008,
        0x0300,
        "PrivateDataElementCharacteristicsSequence",
        "SQ",
        "1",
        "3",
        "SOPCommonModule"
    )
    val InstanceOriginStatus =
        DicomTagInfo(0x0400, 0x0600, "InstanceOriginStatus", "CS", "1", "3", "SOPCommonModule")
    val BarcodeValue =
        DicomTagInfo(0x2200, 0x0005, "BarcodeValue", "LT", "1", "3", "SOPCommonModule")
    val ReferencedDefinedProtocolSequence = DicomTagInfo(
        0x0018,
        0x990C,
        "ReferencedDefinedProtocolSequence",
        "SQ",
        "1",
        "1C",
        "SOPCommonModule"
    )
    val ReferencedPerformedProtocolSequence = DicomTagInfo(
        0x0018,
        0x990D,
        "ReferencedPerformedProtocolSequence",
        "SQ",
        "1",
        "1C",
        "SOPCommonModule"
    )

    // --- CommonInstanceReferenceModule (U) ---
    val ReferencedSeriesSequence = DicomTagInfo(
        0x0008,
        0x1115,
        "ReferencedSeriesSequence",
        "SQ",
        "1",
        "1C",
        "CommonInstanceReferenceModule"
    )
    val StudiesContainingOtherReferencedInstancesSequence = DicomTagInfo(
        0x0008,
        0x1200,
        "StudiesContainingOtherReferencedInstancesSequence",
        "SQ",
        "1",
        "1C",
        "CommonInstanceReferenceModule"
    )

    // --- ImagePixelModule ---
    val Rows = DicomTagInfo(0x0028, 0x0010, "Rows", "US", "1", "1", "ImagePixelModule")
    val Columns = DicomTagInfo(0x0028, 0x0011, "Columns", "US", "1", "1", "ImagePixelModule")
    val BitsAllocated =
        DicomTagInfo(0x0028, 0x0100, "BitsAllocated", "US", "1", "1", "ImagePixelModule")
    val BitsStored = DicomTagInfo(0x0028, 0x0101, "BitsStored", "US", "1", "1", "ImagePixelModule")
    val PixelSpacing =
        DicomTagInfo(0x0028, 0x0030, "PixelSpacing", "DS", "2", "1", "ImagePixelModule")

    /**
     * Get all tags as a map for lookup
     */
    val allTags: Map<Int, DicomTagInfo> by lazy {
        val tags = mutableMapOf<Int, DicomTagInfo>()
        // Using reflection or just manual addition. Manual is safer here.
        val fields = DicomTag::class.java.declaredFields
        for (field in fields) {
            if (field.type == DicomTagInfo::class.java) {
                val tagInfo = field.get(null) as DicomTagInfo
                tags[tagInfo.tag] = tagInfo
            }
        }
        tags
    }
}
