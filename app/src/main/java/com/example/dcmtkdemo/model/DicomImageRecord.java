package com.example.dcmtkdemo.model;

import java.io.File;

/**
 * 用于在 DcmShowFragment 列表与 DetailActivity 详情中传递的 DICOM 文件信息。
 * 包含从 DICOM 标签解析出的关键字段，以及源 dcm 与转换后 jpg 的绝对路径。
 */
public class DicomImageRecord {
    private final String name;
    private final String id;
    private final String sex;
    private final String studyDate;
    private final String studyDesc;
    private final String dcmPath;
    private final String jpgPath;

    public DicomImageRecord(String name, String id, String sex, String studyDate,
                            String studyDesc, String dcmPath, String jpgPath) {
        this.name = name;
        this.id = id;
        this.sex = sex;
        this.studyDate = studyDate;
        this.studyDesc = studyDesc;
        this.dcmPath = dcmPath;
        this.jpgPath = jpgPath;
    }

    public String getName() { return name; }
    public String getId() { return id; }
    public String getSex() { return sex; }
    public String getStudyDate() { return studyDate; }
    public String getStudyDesc() { return studyDesc; }
    public String getDcmPath() { return dcmPath; }
    public String getJpgPath() { return jpgPath; }

    /** 转换后的 jpg 是否已存在（可用于决定是否需要触发批量转换） */
    public boolean hasJpg() {
        return jpgPath != null && new File(jpgPath).exists();
    }
}
