#include "dcmtk/config/osconfig.h"
#include "ProgressScu.h"

ProgressScu::ProgressScu()
        : DcmSCU()
        , m_totalBytes(0)
        , m_lastRecvBytes(0)
        , m_totalRecv(0) {}

void ProgressScu::notifySENDProgress(const unsigned long byteCount) {
    notify(byteCount, m_totalBytes);
    DcmSCU::notifySENDProgress(byteCount);
}

void ProgressScu::notifyRECEIVEProgress(const unsigned long byteCount) {
    if (byteCount < m_lastRecvBytes) {
        m_totalRecv += m_lastRecvBytes;
    }
    m_lastRecvBytes = byteCount;
    notify(m_totalRecv + byteCount, 0);
    DcmSCU::notifyRECEIVEProgress(byteCount);
}

void ProgressScu::notify(unsigned long sent, unsigned long total) {
    if (m_callback) {
        m_callback(sent, total);
    }
}
