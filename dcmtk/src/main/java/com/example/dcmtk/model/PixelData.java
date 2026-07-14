package com.example.dcmtk.model;

public class PixelData {
    private int rows;
    private int columns;
    private byte[] data;
    private int largestImagePixelValue;
    private int win_center;
    private int win_width;
    private final int exposure_leve;
    private final double standardDeviation;

    public PixelData(int rows, int columns, byte[] data, int largestImagePixelValue
            , int win_center, int win_width, int exposure_leve, double standardDeviation) {
        this.rows = rows;
        this.columns = columns;
        this.data = data;
        this.largestImagePixelValue = largestImagePixelValue;
        this.win_center = win_center;
        this.win_width = win_width;
        this.exposure_leve = exposure_leve;
        this.standardDeviation = standardDeviation;
    }

    public double getStandardDeviation() {
        return standardDeviation;
    }

    public int getExposure_leve() { return exposure_leve;}

    public int getRows() {
        return rows;
    }

    public void setRows(int rows) {
        this.rows = rows;
    }

    public int getColumns() {
        return columns;
    }

    public void setColumns(int columns) {
        this.columns = columns;
    }

    public byte[] getData() {
        return data;
    }

    public void setData(byte[] data) {
        this.data = data;
    }
    public int getLargestImagePixelValue() {
        return largestImagePixelValue;
    }
    public void setLargestImagePixelValue(int largestImagePixelValue) {
        this.largestImagePixelValue = largestImagePixelValue;
    }

    public int getWin_center() {
        return win_center;
    }

    public void setWin_center(int win_center) {
        this.win_center = win_center;
    }

    public int getWin_width() {
        return win_width;
    }

    public void setWin_width(int win_width) {
        this.win_width = win_width;
    }
}
