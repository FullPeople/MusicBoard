// MIT: adapted from FullPeople/FullOffice d3f6036; see ORIGIN.md.
#include "BundleExtract.h"
#include "miniz.h"
#include "core/sha256.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <set>
#include <vector>
#include <fstream>
#include <sstream>

namespace fulltools {
namespace {
struct Archive final { mz_zip_archive value{}; ~Archive() { if (value.m_pState) mz_zip_reader_end(&value); } };
struct File final { HANDLE value = INVALID_HANDLE_VALUE; ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct Sink final { HANDLE file; ExtractionProgress* progress; std::uint64_t written = 0; };
std::wstring io_path(const std::filesystem::path& path) {
    std::wstring full = std::filesystem::absolute(path).lexically_normal().native();
    if (full.starts_with(L"\\\\?\\")) return full;
    if (full.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}
size_t write_data(void* context, mz_uint64 offset, const void* bytes, size_t count) {
    auto& sink = *static_cast<Sink*>(context);
    if (sink.progress->cancelled.load() || offset != sink.written) return 0;
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    size_t done = 0;
    while (done < count) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(count - done, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(sink.file, data + done, chunk, &written, nullptr) || written == 0) return 0;
        done += written;
    }
    sink.written += done;
    const auto completed = sink.progress->completed.fetch_add(done) + done;
    if (sink.progress->observer) sink.progress->observer(completed, sink.progress->total.load());
    return done;
}
bool reparse(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(io_path(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
bool safe_directory_chain(const std::filesystem::path& path) {
    if (!path.is_absolute()) return false;
    auto current = path.root_path();
    if (reparse(current)) return false;
    for (const auto& component : path.relative_path()) {
        if (component == L"." || component == L"..") return false;
        current /= component;
        if (reparse(current)) return false;
    }
    return true;
}
bool relative_name(const char* text, std::filesystem::path& result, std::string& error) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (count <= 1) { error = "Archive entry has an empty or invalid UTF-8 name"; return false; }
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide.data(), count);
    wide.resize(static_cast<size_t>(count - 1));
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    if (wide.front() == L'\\' || wide.find(L':') != std::wstring::npos ||
        wide.find_first_of(L"<>\"|?*") != std::wstring::npos) { error = "Archive entry contains an unsafe path"; return false; }
    result = std::filesystem::path(wide);
    for (const auto& part : result) {
        const auto value = part.native();
        if (value.empty() || value == L"." || value == L".." || value.back() == L' ' || value.back() == L'.') {
            error = "Archive entry contains a path traversal or ambiguous component"; return false;
        }
        auto stem = value.substr(0, value.find(L'.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t letter) { return static_cast<wchar_t>(std::towupper(letter)); });
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && stem[3] >= L'1' && stem[3] <= L'9')) {
            error = "Archive entry names a Windows device"; return false;
        }
    }
    return true;
}
bool ancestors_safe(const std::filesystem::path& root, const std::filesystem::path& relative) {
    auto current = root;
    if (reparse(current)) return false;
    for (const auto& part : relative) { current /= part; if (reparse(current)) return false; }
    return true;
}
bool archive_manifest(mz_zip_archive& archive, ExtractionProgress& progress, std::string& error) {
    const int index = mz_zip_reader_locate_file(&archive, "SHA256SUMS.txt", nullptr, 0);
    mz_zip_archive_file_stat info{};
    if (index < 0 || !mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &info) ||
        info.m_uncomp_size == 0 || info.m_uncomp_size > 4 * 1024 * 1024) {
        error = "Package is missing a valid original manifest"; return false;
    }
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &size, 0);
    if (!data) { error = "Cannot read the authenticated package manifest"; return false; }
    musicboard::Failure failure;
    const bool hashed = musicboard::sha256_hex(static_cast<const std::uint8_t*>(data), size,
        progress.manifest_sha256, failure);
    mz_free(data);
    if (!hashed) { error = "Cannot authenticate the package manifest"; return false; }
    return true;
}
}
bool extract_bundle(const std::uint8_t* bytes, const std::size_t length,
    const std::filesystem::path& destination, ExtractionProgress& progress, std::string& error,
    const ExtractionOptions& options) {
    if (!bytes || length == 0 || destination.empty()) { error = "Missing package bytes or destination"; return false; }
    if (!safe_directory_chain(destination)) { error = "Staging path has an unsafe or linked parent directory"; return false; }
    std::error_code status;
    if (std::filesystem::exists(destination, status) || status) { error = "Extraction requires a new staging directory"; return false; }
    Archive archive;
    if (!mz_zip_reader_init_mem(&archive.value, bytes, length, 0)) { error = "Package archive is invalid"; return false; }
    struct Entry { mz_uint index; std::filesystem::path relative; std::uint64_t bytes; bool directory; };
    std::vector<Entry> entries;
    std::set<std::wstring> names;
    std::uint64_t total = 0, archive_total = 0;
    const mz_uint count = mz_zip_reader_get_num_files(&archive.value);
    if (count == 0 || count > 10000) { error = "Archive file count exceeds the supported limit"; return false; }
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat info{};
        if (!mz_zip_reader_file_stat(&archive.value, index, &info)) { error = "Archive directory cannot be read"; return false; }
        const unsigned mode = (info.m_external_attr >> 16) & 0170000;
        if (mode == 0120000 || (info.m_external_attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || info.m_is_encrypted || info.m_is_supported == 0) {
            error = "Archive contains an unsupported, encrypted, or linked entry"; return false;
        }
        std::filesystem::path relative;
        const mz_uint filename_bytes = mz_zip_reader_get_filename(&archive.value, index, nullptr, 0);
        if (filename_bytes < 2 || filename_bytes > 32768) { error = "Archive filename length is invalid"; return false; }
        std::vector<char> filename(filename_bytes);
        if (mz_zip_reader_get_filename(&archive.value, index, filename.data(), filename_bytes) != filename_bytes ||
            std::find(filename.begin(), filename.end() - 1, '\0') != filename.end() - 1) {
            error = "Archive filename is truncated or contains an embedded null"; return false;
        }
        std::string name(filename.data(), filename.size() - 1);
        const bool directory = info.m_is_directory != 0 || name.ends_with('/') || name.ends_with('\\');
        if (directory && info.m_uncomp_size != 0) { error = "Archive directory contains unexpected data"; return false; }
        while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
        if (!relative_name(name.c_str(), relative, error)) return false;
        std::wstring folded = relative.native();
        std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
        if (!names.insert(folded).second) { error = "Archive contains duplicate paths"; return false; }
        if (info.m_uncomp_size > 2ULL * 1024 * 1024 * 1024 || archive_total > 8ULL * 1024 * 1024 * 1024 - info.m_uncomp_size) {
            error = "Archive exceeds the extraction size limit"; return false;
        }
        archive_total += info.m_uncomp_size;
        total += info.m_uncomp_size;
        entries.push_back({index, relative, info.m_uncomp_size, directory});
    }
    ULARGE_INTEGER free{};
    if (!GetDiskFreeSpaceExW(destination.parent_path().c_str(), &free, nullptr, nullptr) || free.QuadPart < total + 64ULL * 1024 * 1024) {
        error = "Insufficient staging disk space"; return false;
    }
    if (!std::filesystem::create_directory(destination, status) || status || !safe_directory_chain(destination)) {
        error = "Cannot create a private extraction directory"; return false;
    }
    progress.total.store(total);
    progress.completed.store(0);
    progress.omitted_bytes.store(0);
    progress.files.store(0);
    if (progress.observer) progress.observer(0, total);
    for (const auto& entry : entries) {
        if (progress.cancelled.load()) { error = "Extraction cancelled"; return false; }
        if (!ancestors_safe(destination, entry.relative)) { error = "Staging directory was replaced by a link"; return false; }
        const auto path = destination / entry.relative;
        std::filesystem::create_directories(io_path(entry.directory ? path : path.parent_path()), status);
        if (status || !ancestors_safe(destination, entry.relative)) {
            const auto name = entry.relative.generic_u8string();
            error = "Cannot create archive directory: " + std::string(name.begin(), name.end()) + " error=" + std::to_string(status.value()); return false;
        }
        if (entry.directory) continue;
        File file;
        // Wizard staging is disposable; the backend reopens and hashes every file
        // before committing. Per-file durable flushes are optional here.
        file.value = CreateFileW(io_path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) {
            const auto name = entry.relative.generic_u8string();
            error = "Cannot create staged package file: " + std::string(name.begin(), name.end()) + " Win32=" + std::to_string(GetLastError()); return false;
        }
        Sink sink{file.value, &progress};
        if (!mz_zip_reader_extract_to_callback(&archive.value, entry.index, write_data, &sink, 0) || sink.written != entry.bytes ||
            (options.flush_staged_files && !FlushFileBuffers(file.value))) {
            error = progress.cancelled.load() ? "Extraction cancelled" : "Archive contents are damaged or cannot be written";
            return false;
        }
        progress.files.fetch_add(1);
    }
    if (!archive_manifest(archive.value, progress, error)) return false;
    return true;
}
bool remove_owned_stage(const std::filesystem::path& destination,
    const std::filesystem::path& expected_parent, std::string& error) {
    std::error_code status;
    if (!safe_directory_chain(destination) || !safe_directory_chain(expected_parent)) {
        error = "Refusing cleanup through a linked or unsafe parent directory"; return false;
    }
    const auto name = destination.filename().native();
    const auto normalized = std::filesystem::weakly_canonical(destination, status);
    if (status) { error = "Cannot resolve staging path"; return false; }
    const auto parent = std::filesystem::weakly_canonical(expected_parent, status);
    if (status || normalized.parent_path() != parent || !name.starts_with(L"FullTools-stage-") ||
        destination.is_relative() || expected_parent.is_relative() || reparse(destination)) {
        error = "Refusing cleanup outside an owned staging directory"; return false;
    }
    if (!std::filesystem::exists(destination, status)) return !status;
    auto iterator = std::filesystem::recursive_directory_iterator(destination, status);
    const auto end = std::filesystem::recursive_directory_iterator();
    if (status) { error = "Cannot inspect staging contents for cleanup"; return false; }
    while (iterator != end) {
        if (reparse(iterator->path())) { error = "Refusing cleanup of a linked staging directory"; return false; }
        iterator.increment(status);
        if (status) { error = "Cannot inspect staging contents for cleanup"; return false; }
    }
    std::filesystem::remove_all(destination, status);
    if (status) { error = "Cannot remove owned staging directory: " + status.message(); return false; }
    return true;
}
}
