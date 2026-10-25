#pragma once

namespace parties {
class LogSession;
}

namespace parties::alloctrack {

class ReportingSession {
public:
#ifdef PARTIES_ALLOC_TRACKER
    ReportingSession(const LogSession& log, unsigned interval_seconds);
    ~ReportingSession();
#else
    ReportingSession(const LogSession&, unsigned) {}
#endif
    ReportingSession(const ReportingSession&) = delete;
    ReportingSession& operator=(const ReportingSession&) = delete;
};

}
