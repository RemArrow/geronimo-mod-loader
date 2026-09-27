#include "internal.h"
#include <cstdarg>
#include <cstdio>

namespace gml {

static SRWLOCK      s_lock = SRWLOCK_INIT;
static HANDLE       s_file = INVALID_HANDLE_VALUE;
static GML_LogLevel s_min  = GML_LOG_INFO;
static bool         s_console = false;
static bool         s_timestamps = false;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

int CopyOut(const std::string& s, char* buf, int cap) {
    if (buf && cap > 0) {
        size_t n = s.size() < (size_t)(cap - 1) ? s.size() : (size_t)(cap - 1);
        memcpy(buf, s.data(), n);
        buf[n] = 0;
    }
    return (int)s.size();
}

void LogInit(const std::wstring& path, GML_LogLevel minLevel, bool console, bool timestamps) {
    s_min = minLevel;
    s_timestamps = timestamps;
    s_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (console && AllocConsole()) {
        s_console = true;
        SetConsoleTitleW(L"Geronimo Mod Loader");
        SetConsoleOutputCP(CP_UTF8);
    }
}

void LogWrite(GML_LogLevel lvl, const char* source, const char* msg) {
    if (lvl < s_min) return;
    static const char* names[] = {"Debug", "Info", "Message", "Warning", "Error", "Fatal"};
    char stamp[32] = "";
    if (s_timestamps) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        snprintf(stamp, sizeof stamp, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    }
    char line[2400];
    int n = snprintf(line, sizeof line, "%s[%-7s:%10s] %s\r\n", stamp, names[lvl < 0 || lvl > 5 ? 1 : lvl],
                     source ? source : "?", msg ? msg : "");
    if (n < 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;

    AcquireSRWLockExclusive(&s_lock);
    DWORD w;
    // Unbuffered WriteFile lands in the OS cache immediately, so lines survive a game crash.
    if (s_file != INVALID_HANDLE_VALUE) WriteFile(s_file, line, n, &w, nullptr);
    if (s_console) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, n, &w, nullptr);
    ReleaseSRWLockExclusive(&s_lock);
}

void Log(GML_LogLevel lvl, const char* fmt, ...) {
    if (lvl < s_min) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    LogWrite(lvl, "GML", msg);
}

}  // namespace gml
