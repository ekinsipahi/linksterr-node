// util.h — small shared helpers: logging, UTF-8/UTF-16, files, hashing.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace util {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

extern Level g_level;

void log(Level level, const char* tag, const char* fmt, ...);

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

// %LOCALAPPDATA%\LinksterrNode, created on first use. Tokens and the consent
// record live here, per Windows user.
std::wstring data_dir();
bool read_file(const std::wstring& path, std::string& out);
bool write_file(const std::wstring& path, const std::string& data);
bool remove_file(const std::wstring& path);
bool file_exists(const std::wstring& path);

uint64_t fnv1a64(const std::string& s);
std::string hex64(uint64_t v);
std::string trim(const std::string& s);
std::string now_iso8601();
std::string os_description();
std::string win_error(DWORD code);
std::string human_bytes(uint64_t n);

}  // namespace util

#define LOGD(tag, ...) ::util::log(::util::Level::Debug, tag, __VA_ARGS__)
#define LOGI(tag, ...) ::util::log(::util::Level::Info, tag, __VA_ARGS__)
#define LOGW(tag, ...) ::util::log(::util::Level::Warn, tag, __VA_ARGS__)
#define LOGE(tag, ...) ::util::log(::util::Level::Error, tag, __VA_ARGS__)
