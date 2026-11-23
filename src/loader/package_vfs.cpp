#include "scarlettx/loader/package_vfs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

std::string PackageVfs::NormalizeVirtualPath(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

std::string PackageVfs::NormalizeKey(const std::string& path) {
    std::string key = NormalizeVirtualPath(path);
    while (!key.empty() && key.back() == '\\') {
        key.pop_back();
    }
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return key;
}

Status PackageVfs::MountDirectory(const std::string& virtual_root, const std::string& host_dir) {
    if (virtual_root.empty() || host_dir.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Empty virtual_root or host_dir");
    }
    if (!std::filesystem::exists(host_dir)) {
        return Status::Error(StatusCode::NotFound, "Host directory does not exist: " + host_dir);
    }
    mounts_[NormalizeKey(virtual_root)] = host_dir;
    return Status::Ok();
}

Status PackageVfs::MountXvd(const std::string& virtual_path, const std::string& xvd_host_path) {
    if (virtual_path.empty() || xvd_host_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Empty virtual_path or xvd_host_path");
    }
    auto container = std::make_shared<XvdContainer>();
    Status st = container->OpenFile(xvd_host_path);
    if (!st.ok()) {
        return st;
    }
    if (container->Header().is_encrypted) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "Encrypted XVD containers cannot be mounted without decryption");
    }
    xvd_mounts_[NormalizeKey(virtual_path)] = std::move(container);
    return Status::Ok();
}

Status PackageVfs::ResolvePath(const std::string& virtual_path, std::string& out_host_path) const {
    std::string norm = NormalizeVirtualPath(virtual_path);
    std::string lower = norm;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // A nested directory mount takes precedence over its parent drive mount.
    const std::string* host_dir = nullptr;
    size_t root_length = 0;
    for (const auto& [root, directory] : mounts_) {
        if ((!host_dir || root.size() > root_length) && lower.starts_with(root) &&
            (lower.size() == root.size() || lower[root.size()] == '\\')) {
            host_dir = &directory;
            root_length = root.size();
        }
    }
    if (!host_dir) {
        return Status::Error(StatusCode::NotFound, "No mount point for virtual path: " + virtual_path);
    }

    const size_t rel_start = norm.find_first_not_of('\\', root_length);
    const std::string rel = rel_start == std::string::npos ? std::string{} : norm.substr(rel_start);
    out_host_path = (std::filesystem::path(*host_dir) / rel).string();
    return Status::Ok();
}

Status PackageVfs::ReadFileBytes(const std::string& virtual_path,
                                 std::vector<uint8_t>& out_data) const {
    const std::string key = NormalizeKey(virtual_path);
    auto xit = xvd_mounts_.find(key);
    if (xit != xvd_mounts_.end()) {
        const auto& hdr = xit->second->Header();
        return xit->second->ReadUserData(0, hdr.user_data_length, out_data);
    }
    std::string host_path;
    Status st = ResolvePath(virtual_path, host_path);
    if (!st.ok()) {
        return st;
    }
    std::ifstream ifs(host_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
    }
    out_data.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return Status::Ok();
}

Status PackageVfs::ReadFileRange(const std::string& virtual_path,
                                 uint64_t offset,
                                 uint32_t size,
                                 std::vector<uint8_t>& out_data) const {
    out_data.clear();
    const std::string key = NormalizeKey(virtual_path);
    auto xit = xvd_mounts_.find(key);
    if (xit != xvd_mounts_.end()) {
        const auto& hdr = xit->second->Header();
        if (offset > hdr.user_data_length ||
            size > hdr.user_data_length - offset) {
            return Status::Error(StatusCode::InvalidArgument, "XVD read range out of bounds");
        }
        return xit->second->ReadUserData(offset, size, out_data);
    }

    std::string host_path;
    Status st = ResolvePath(virtual_path, host_path);
    if (!st.ok()) {
        return st;
    }
#if defined(_WIN32)
    HANDLE handle = CreateFileA(host_path.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
    }
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(handle, &file_size) ||
        offset > static_cast<uint64_t>(file_size.QuadPart) ||
        size > static_cast<uint64_t>(file_size.QuadPart) - offset) {
        CloseHandle(handle);
        return Status::Error(StatusCode::InvalidArgument, "Mounted file read range out of bounds");
    }
    out_data.resize(size);
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        CloseHandle(handle);
        out_data.clear();
        return Status::Error(StatusCode::IoError, "Failed to create overlapped file event");
    }
    BOOL ok = ReadFile(handle, out_data.data(), size, &read, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = WaitForSingleObject(overlapped.hEvent, 30000) == WAIT_OBJECT_0;
        if (ok) {
            ok = GetOverlappedResult(handle, &overlapped, &read, TRUE);
        } else {
            CancelIoEx(handle, &overlapped);
        }
    }
    CloseHandle(overlapped.hEvent);
    CloseHandle(handle);
    if (!ok || read != size) {
        out_data.clear();
        return Status::Error(StatusCode::IoError, "Mounted file range read failed: " + host_path);
    }
    return Status::Ok();
#else
    std::ifstream ifs(host_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
    }
    ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    out_data.resize(size);
    ifs.read(reinterpret_cast<char*>(out_data.data()), static_cast<std::streamsize>(size));
    if (static_cast<uint32_t>(ifs.gcount()) != size) {
        out_data.clear();
        return Status::Error(StatusCode::IoError, "Mounted file range read failed: " + host_path);
    }
    return Status::Ok();
#endif
}

bool PackageVfs::Exists(const std::string& virtual_path) const {
    if (xvd_mounts_.find(NormalizeKey(virtual_path)) != xvd_mounts_.end()) {
        return true;
    }
    std::string host_path;
    if (!ResolvePath(virtual_path, host_path).ok()) {
        return false;
    }
    return std::filesystem::exists(host_path);
}

}  // namespace scarlettx
