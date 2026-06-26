package com.example.dcmtkdemo.model;

public class PatientRecord {
    private String name;
    private String id;
    private String sex;
    private String birthDate;

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
}
