#ifndef DCMTKDEMO_DICOMFILEIO_H
#define DCMTKDEMO_DICOMFILEIO_H

#include <map>
#include <string>

// DICOM file operations: tag reading, raw->DICOM writing, and DICOM->JPG
// conversion. Pure C++ interface, independent of JNI.
class DicomFileIO {
public:
    // Load every leaf element of a DICOM file into a map keyed by "(grp,elem)".
    // Returns an empty map on failure (load error / null path).
    static std::map<std::string, std::string> loadFileInfo(const std::string &filePath);

    // Build a DICOM file from a raw 16-bit pixel buffer at `rawPath`, writing
    // the result to `dcmPath`. Computes min/max window center/width. Returns
    // true on success.
    static bool writeDicomFile(const std::string &rawPath, const std::string &dcmPath,
                               int width, int height);

    // Convert every non-JPG file in `dir` to `dir/jpg/<name>.jpg` using min/max
    // windowing for monochrome images. Returns the number of files converted.
    static int dcmToJpg(const std::string &dir);
};

#endif // DCMTKDEMO_DICOMFILEIO_H
