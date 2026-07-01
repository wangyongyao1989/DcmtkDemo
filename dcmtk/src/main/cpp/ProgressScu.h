#ifndef DCMTKDEMO_PROGRESSSCU_H
#define DCMTKDEMO_PROGRESSSCU_H

#include <functional>

#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmnet/scu.h"

// A DcmSCU subclass that reports send/receive progress via a std::function
// callback instead of JNI, keeping it decoupled from Java. The JNI bridge
// layer supplies a lambda that forwards to ProgressCallback.onProgress().
//
// Thread contract: the callback is invoked synchronously on the same thread
// that calls sendSTORERequest()/sendCGETRequest(), so capturing JNIEnv* and
// a local jobject reference in the lambda is safe (matches the original
// ProgressSCU semantics; no global references are needed).
class ProgressScu : public DcmSCU {
public:
    using ProgressCallback = std::function<void(unsigned long sent, unsigned long total)>;

    ProgressScu();

    // Total bytes known for C-STORE uploads (file size); 0 for C-GET (unknown).
    void setTotalBytes(unsigned long total) { m_totalBytes = total; }
    void setProgressCallback(ProgressCallback cb) { m_callback = std::move(cb); }

protected:
    void notifySENDProgress(const unsigned long byteCount) override;

    // C-GET download: byteCount is cumulative for the CURRENT file being
    // received. It resets to a small value when a new file starts. We detect
    // the reset and accumulate across files to report total bytes received.
    void notifyRECEIVEProgress(const unsigned long byteCount) override;

private:
    unsigned long m_totalBytes;    // C-STORE: known file size; C-GET: 0 (unknown)
    unsigned long m_lastRecvBytes; // last byteCount seen for current file
    unsigned long m_totalRecv;     // accumulated bytes from completed files
    ProgressCallback m_callback;

    void notify(unsigned long sent, unsigned long total);
};

#endif // DCMTKDEMO_PROGRESSSCU_H
