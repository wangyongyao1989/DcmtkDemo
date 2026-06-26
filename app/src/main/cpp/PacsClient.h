#ifndef DCMTKDEMO_PACSCLIENT_H
#define DCMTKDEMO_PACSCLIENT_H

#include <functional>
#include <string>
#include <vector>

// PACS network operations (DICOM SCU). All methods are blocking and use
// pure C++ types, so this class is independent of JNI and can be read/tested
// in isolation. The JNI bridge layer handles Java<->C++ marshaling.
class PacsClient {
public:
    using ProgressCallback = std::function<void(unsigned long sent, unsigned long total)>;

    // Test-only association: negotiate then immediately release.
    static bool connectPACS(const std::string &host, int port,
                            const std::string &localAet, const std::string &remoteAet);

    // C-ECHO verification.
    static bool cEcho(const std::string &host, int port,
                      const std::string &localAet, const std::string &remoteAet);

    // C-STORE: upload a single DICOM file. `callback` reports byte progress
    // (sent bytes vs known file size); may be null.
    static bool cStore(const std::string &host, int port,
                       const std::string &localAet, const std::string &remoteAet,
                       const std::string &dcmPath, ProgressCallback callback);

    // C-FIND by patient name. Returns one summary string per match,
    // formatted as "name | ID:id | sex | birth".
    static std::vector<std::string> cFind(const std::string &host, int port,
                                          const std::string &localAet,
                                          const std::string &remoteAet,
                                          const std::string &patientName);

    // C-MOVE: ask the PACS to push matching instances to `destAet`.
    static bool cMove(const std::string &host, int port,
                      const std::string &localAet, const std::string &remoteAet,
                      const std::string &patientId, const std::string &destAet);

    // C-GET: download matching instances directly into `saveDir`.
    // `callback` reports cumulative bytes received; may be null.
    static bool cGet(const std::string &host, int port,
                     const std::string &localAet, const std::string &remoteAet,
                     const std::string &patientId, const std::string &saveDir,
                     ProgressCallback callback);
};

#endif // DCMTKDEMO_PACSCLIENT_H
