package com.example.dcmtkdemo.model;

public class PatientRecord {
    private String name;
    private String id;
    private String sex;
    private String birthDate;
    private boolean isSelected = false;
    private boolean isDownloaded = false;

    public PatientRecord(String name, String id, String sex, String birthDate) {
        this.name = name;
        this.id = id;
        this.sex = sex;
        this.birthDate = birthDate;
    }

    public String getName() { return name; }
    public String getId() { return id; }
    public String getSex() { return sex; }
    public String getBirthDate() { return birthDate; }

    public boolean isSelected() { return isSelected; }
    public void setSelected(boolean selected) { isSelected = selected; }

    public boolean isDownloaded() { return isDownloaded; }
    public void setDownloaded(boolean downloaded) { isDownloaded = downloaded; }
}
