package com.example.dcmtkdemo.model;

public class PatientRecord {
    private String name;
    private String id;
    private String sex;
    private String birthDate;
    private String accessionNumber = "";
    private String modality = "";
    private boolean isSelected = false;
    private boolean isDownloaded = false;

    public PatientRecord(String name, String id, String sex, String birthDate) {
        this.name = name;
        this.id = id;
        this.sex = sex;
        this.birthDate = birthDate;
    }

    public PatientRecord(String name, String id, String sex, String birthDate, String accessionNumber, String modality) {
        this.name = name;
        this.id = id;
        this.sex = sex;
        this.birthDate = birthDate;
        this.accessionNumber = accessionNumber;
        this.modality = modality;
    }

    public String getName() { return name; }
    public String getId() { return id; }
    public String getSex() { return sex; }
    public String getBirthDate() { return birthDate; }
    public String getAccessionNumber() { return accessionNumber; }
    public String getModality() { return modality; }

    public boolean isSelected() { return isSelected; }
    public void setSelected(boolean selected) { isSelected = selected; }

    public boolean isDownloaded() { return isDownloaded; }
    public void setDownloaded(boolean downloaded) { isDownloaded = downloaded; }
    
    @Override
    public String toString() {
        return name + " (" + id + ")" + (accessionNumber.isEmpty() ? "" : " Acc:" + accessionNumber);
    }
}
