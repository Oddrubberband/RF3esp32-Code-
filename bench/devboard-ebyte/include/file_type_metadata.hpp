#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace FileTypeMetadata {
constexpr size_t kExtensionBytes = 10;  // Includes the leading dot, no truncation.
constexpr uint8_t kStartExtensionFlag = 1;
using Extension = std::array<char, kExtensionBytes>;

inline bool empty(const Extension& field)
{
    return std::all_of(field.begin(), field.end(), [](char byte) { return byte == 0; });
}
inline std::string_view view(const Extension& field)
{
    const auto end = std::find(field.begin(), field.end(), '\0');
    return {field.data(), static_cast<size_t>(end - field.begin())};
}
inline bool valid(std::string_view extension)
{
    if (extension.size() < 2 || extension.size() > kExtensionBytes || extension.front() != '.') return false;
    for (const unsigned char byte : extension.substr(1)) {
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9') || byte == '_' || byte == '-')) return false;
    }
    const auto lower = [](char byte) { return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : byte; };
    if (extension.size() == 5 && lower(extension[1]) == 'p' && lower(extension[2]) == 'a' &&
        lower(extension[3]) == 'r' && lower(extension[4]) == 't') return false;
    return true;
}
inline bool canonical(const Extension& field, bool allow_empty = true)
{
    const auto extension = view(field);
    if (extension.empty()) return allow_empty && empty(field);
    if (!valid(extension)) return false;
    return std::all_of(field.begin() + extension.size(), field.end(), [](char byte) { return byte == 0; });
}
inline bool derive(std::string_view logical_name, Extension& out)
{
    out.fill(0);
    const size_t separator = logical_name.find_last_of("/\\");
    const size_t base = separator == std::string_view::npos ? 0 : separator + 1;
    const size_t dot = logical_name.find_last_of('.');
    if (dot == std::string_view::npos || dot <= base) return true;  // Extensionless/hidden basename.
    const auto extension = logical_name.substr(dot);
    if (!valid(extension)) return false;
    std::copy(extension.begin(), extension.end(), out.begin());
    return true;
}
inline bool startMetadataValid(uint8_t flags, const Extension& extension)
{
    return flags == 0 ? empty(extension) :
           flags == kStartExtensionFlag && canonical(extension, false);
}
inline std::string receivedName(uint32_t id, bool partial, uint16_t collision,
                                std::string_view extension = {})
{
    if (!extension.empty() && !valid(extension)) return {};
    const std::string suffix = partial ? ".part" : extension.empty() ? ".bin" : std::string(extension);
    char base[24]{};
    if (partial || collision == 0) std::snprintf(base, sizeof(base), "rx_%08lX", static_cast<unsigned long>(id));
    else std::snprintf(base, sizeof(base), "rx_%08lX_%03u", static_cast<unsigned long>(id), static_cast<unsigned>(collision));
    return std::string(base) + suffix;
}
}  // namespace FileTypeMetadata
