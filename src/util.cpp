#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace util {

Level g_level = Level::Info;

static std::mutex g_log_mu;

static void emit(Level level, const char* tag, const char* body, WORD colour) {
    static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(g_log_mu);
    HANDLE con = GetStdHandle(STD_ERROR_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    bool painted = colour && GetConsoleScreenBufferInfo(con, &info);  // false when redirected to a file
    fprintf(stderr, "%02d:%02d:%02d %s [%s] ", st.wHour, st.wMinute, st.wSecond,
            names[static_cast<int>(level)], tag);
    fflush(stderr);
    if (painted) SetConsoleTextAttribute(con, colour);
    fprintf(stderr, "%s\n", body);
    fflush(stderr);
    if (painted) SetConsoleTextAttribute(con, info.wAttributes);
}

void log(Level level, const char* tag, const char* fmt, ...) {
    if (level < g_level) return;
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    emit(level, tag, body, 0);
}

void log_painted(Level level, Paint paint, const char* tag, const char* fmt, ...) {
    if (level < g_level) return;
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    WORD colour = paint == Paint::Red ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                                      : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    emit(level, tag, body, colour);
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring data_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : L".";
    std::wstring dir = base + L"\\LinksterrNode";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool read_file(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return true;
}

bool write_file(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr);
    CloseHandle(h);
    return ok && put == data.size();
}

bool remove_file(const std::wstring& path) { return DeleteFileW(path.c_str()) != 0; }

bool file_exists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

std::string hex64(uint64_t v) {
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string now_iso8601() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string os_description() {
    // GetVersionEx lies to un-manifested apps; RtlGetVersion does not.
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof info;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion")) fn(&info);
    }
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                     : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86";
    char buf[64];
    snprintf(buf, sizeof buf, "Windows %lu.%lu.%lu %s", info.dwMajorVersion, info.dwMinorVersion,
             info.dwBuildNumber, arch);
    return buf;
}

std::string win_error(DWORD code) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), code, 0, (LPWSTR)&msg, 0, nullptr);
    std::string text = n ? trim(narrow(std::wstring(msg, n))) : "";
    if (msg) LocalFree(msg);
    char buf[32];
    snprintf(buf, sizeof buf, " (code %lu)", code);
    return (text.empty() ? "error" : text) + buf;
}

std::string human_bytes(uint64_t n) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)n;
    int u = 0;
    while (v >= 1000.0 && u < 4) { v /= 1000.0; ++u; }
    char buf[32];
    snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.2f %s", v, units[u]);
    return buf;
}

}  // namespace util
