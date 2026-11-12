#include "minetrack.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

class CacheLock {
public:
    explicit CacheLock(const fs::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot lock download cache: " + path_utf8(path));
#else
        fd_ = open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (fd_ < 0 || flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0) close(fd_);
            throw std::runtime_error("Cannot lock download cache: " + path_utf8(path));
        }
#endif
    }
    ~CacheLock() {
#ifdef _WIN32
        CloseHandle(handle_);
#else
        close(fd_);
#endif
    }
    CacheLock(const CacheLock&) = delete;
    CacheLock& operator=(const CacheLock&) = delete;
private:
#ifdef _WIN32
    HANDLE handle_;
#else
    int fd_;
#endif
};

#ifdef _WIN32
using FileHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

FileHandle child_standard_handle(DWORD channel, DWORD access, SECURITY_ATTRIBUTES& inheritable) {
    HANDLE source = GetStdHandle(channel);
    HANDLE copy = nullptr;
    if (source && source != INVALID_HANDLE_VALUE) {
        HANDLE parent = GetCurrentProcess();
        if (!DuplicateHandle(parent, source, parent, &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("Cannot duplicate a standard handle for a child process");
    } else {
        copy = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (copy == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open NUL for a child process");
    }
    return FileHandle(copy, CloseHandle);
}

std::wstring quote_argument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(2 * slashes, L'\\');
    return result + L'"';
}
#endif

} // namespace

int run_tool(const std::vector<std::string>& arguments, const fs::path& stdout_file) {
#ifdef _WIN32
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command += L' ';
        command += quote_argument(path_from_utf8(argument).native());
    }
    PROCESS_INFORMATION process{};
    {
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const HANDLE file = CreateFileW(stdout_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                        &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open " + path_utf8(stdout_file));
        const FileHandle output(file, CloseHandle);
        const auto error_output = child_standard_handle(STD_ERROR_HANDLE, GENERIC_WRITE, inheritable);
        const auto input = child_standard_handle(STD_INPUT_HANDLE, GENERIC_READ, inheritable);
        // Concurrent launches must not inherit CSV readers or another worker's output.
        HANDLE child_handles[]{output.get(), error_output.get(), input.get()};
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> buffer(bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
            throw std::runtime_error("Cannot initialize child process attributes");
        const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>,
                              decltype(&DeleteProcThreadAttributeList)>
            attribute_list(attributes, DeleteProcThreadAttributeList);
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       child_handles, sizeof(child_handles), nullptr, nullptr))
            throw std::runtime_error("Cannot restrict child process handle inheritance");

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = output.get();
        startup.StartupInfo.hStdError = error_output.get();
        startup.StartupInfo.hStdInput = input.get();
        startup.lpAttributeList = attributes;
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                            &startup.StartupInfo, &process);
        if (!started) {
            const DWORD error = GetLastError();
            throw std::runtime_error("Cannot start " + arguments.front() + " (Windows error " +
                                     std::to_string(error) + "). Check PATH.");
        }
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    const BOOL got_code = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !got_code)
        throw std::runtime_error("Cannot get exit status from " + arguments.front());
    return static_cast<int>(code);
#else
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
        throw std::runtime_error("Cannot initialize process file actions");
    int status = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_file.c_str(),
                                                   O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid;
    if (status == 0)
        status = posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0)
        throw std::runtime_error("Cannot start " + arguments.front() + ": " +
                                 std::error_code(status, std::generic_category()).message());
    int result;
    while (waitpid(pid, &result, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("Cannot wait for " + arguments.front());
    }
    return WIFEXITED(result) ? WEXITSTATUS(result) : 1;
#endif
}

namespace {

#ifdef _WIN32
constexpr auto curl_tool = "curl.exe";
constexpr auto tar_tool = "tar.exe";
#else
constexpr auto curl_tool = "curl";
#ifdef __APPLE__
constexpr auto tar_tool = "tar";
#else
constexpr auto tar_tool = "bsdtar";
#endif
#endif

fs::path part_path(fs::path file) {
    file += ".part";
    return file;
}

enum class Download { ready, missing, empty };

std::string_view unavailable_reason(Download result) {
    return result == Download::missing ? "HTTP 404" : "empty file";
}

Download download(const std::string& url, const fs::path& file) {
    if (fs::exists(file)) {
        if (fs::file_size(file) != 0) return Download::ready;
        fs::remove(file);
    }
    const fs::path part = part_path(file);
    fs::path status_file = file;
    status_file += ".http-status";
    const auto request = [&](bool resume) {
        std::vector<std::string> arguments{
            curl_tool, "--disable", "--globoff", "--fail", "--silent", "--show-error", "--location",
            "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "20",
            "--speed-limit", "1024", "--speed-time", "60", "--retry", "3", "--retry-connrefused",
            "--output", path_utf8(part), "--write-out", "%{http_code}"
        };
        if (resume) {
            arguments.emplace_back("--continue-at");
            arguments.emplace_back("-");
        }
        arguments.push_back(url);
        const int exit = run_tool(arguments, status_file);
        int http = 0;
        std::ifstream(status_file) >> http;
        fs::remove(status_file);
        return std::pair{exit, http};
    };

    const bool resume = fs::exists(part) && fs::file_size(part) != 0;
    auto [exit, http] = request(resume);
    if (resume && (exit == 33 || http == 416)) {
        fs::remove(part);
        std::tie(exit, http) = request(false);
    }
    if (http == 404) return Download::missing;
    if (exit != 0 || (http != 200 && http != 206))
        throw std::runtime_error(url + " download failed (curl " + std::to_string(exit) +
                                 ", HTTP " + std::to_string(http) + ")");
    if (!fs::exists(part) || fs::file_size(part) == 0) {
        fs::remove(part);
        return Download::empty;
    }
    fs::rename(part, file);
    return Download::ready;
}

void extract_sqlite(const fs::path& zip, const std::string& member, const fs::path& output) {
    const auto part = part_path(output);
    const int exit = run_tool({tar_tool, "-xOf", path_utf8(zip), "--", member}, part);
    if (exit != 0 || fs::file_size(part) == 0)
        throw std::runtime_error("Could not extract " + member + " from " + path_utf8(zip));
    fs::rename(part, output);
}

struct DailyArchive {
    fs::path input;
    fs::path zip;
    std::string unavailable;
};

DailyArchive fetch_daily(const fs::path& cache, const std::string& stem) {
    const std::string base = "https://dl.minetrack.me/Java/" + stem;
    const auto csv = cache / (stem + ".csv");
    const auto sqlite = cache / (stem + ".sql");
    const auto zip = cache / (stem + ".sql.zip");
    if (fs::exists(sqlite)) return {sqlite, zip, {}};

    if (!fs::exists(zip) || fs::file_size(zip) == 0) {
        const auto csv_status = download(base + ".csv", csv);
        if (csv_status == Download::ready) return {csv, {}, {}};
        const auto zip_status = download(base + ".sql.zip", zip);
        if (zip_status != Download::ready)
            return {{}, {}, "CSV: " + std::string(unavailable_reason(csv_status)) +
                             "; SQLite ZIP: " + std::string(unavailable_reason(zip_status))};
    }

    extract_sqlite(zip, "Minetrack/database_copy_" + stem + ".sql", sqlite);
    return {sqlite, zip, {}};
}

void legacy(Database& db, const fs::path& cache, const std::string& member, bool keep) {
    const std::string archive = member + ".zip";
    const std::string source = "java:legacy:" + archive;
    if (db.imported(source)) return;

    const auto input = cache / member;
    const auto zip = cache / archive;
    if (!fs::exists(input)) {
        std::cerr << "Fetching " << archive << " (large historical archive)\n";
        const auto status = download("https://dl.minetrack.me/" + archive, zip);
        if (status != Download::ready)
            throw std::runtime_error("Archive unavailable: " + archive + " (" +
                                     std::string(unavailable_reason(status)) + ")");
        std::cerr << "Extracting " << member << '\n';
        extract_sqlite(zip, member, input);
    }
    db.import_sqlite(input, source);
    if (!keep) {
        fs::remove(zip);
        fs::remove(input);
    }
}

} // namespace

int sync_java(Database& db, const fs::path& cache, Day from, Day to, bool keep_downloads,
              unsigned download_jobs) {
    using namespace std::chrono;
    fs::create_directories(cache);
    const CacheLock lock(cache / "sync.lock");
    if (from <= Day(year{2020}/4/3) && to >= first_day)
        legacy(db, cache, "minetrack_java_db.sql", keep_downloads);
    if (from <= Day(year{2020}/7/30) && to >= Day(year{2020}/4/2))
        legacy(db, cache, "minetrack_java_db_2.sql", keep_downloads);
    if (from <= first_daily - days{1} && to >= first_daily - days{1})
        std::cerr << "2020-07-31: between the published legacy and daily ranges. Check coverage.\n";

    int missing = 0;
    int imported = 0;
    int skipped = 0;
    struct PendingArchive {
        std::string date;
        std::string source;
        std::future<DailyArchive> ready;
    };
    std::deque<PendingArchive> pending;
    Day next = std::max(from, first_daily);
    const auto prefetch = [&] {
        while (next <= to && pending.size() < download_jobs) {
            const year_month_day d{next};
            const std::string date = iso_day(next);
            next += days{1};
            const std::string source = "java:daily:" + date;
            if (db.imported(source)) {
                ++skipped;
                continue;
            }
            const std::string stem = std::to_string(unsigned(d.day())) + "-" +
                                     std::to_string(unsigned(d.month())) + "-" +
                                     std::to_string(int(d.year()));
            pending.push_back({date, source, std::async(std::launch::async, [&cache, stem] {
                return fetch_daily(cache, stem);
            })});
        }
    };
    prefetch();
    while (!pending.empty()) {
        PendingArchive archive = std::move(pending.front());
        pending.pop_front();
        const DailyArchive files = archive.ready.get();
        prefetch();
        if (!files.unavailable.empty()) {
            std::cerr << "Missing daily archive " << archive.date << " (" << files.unavailable << ")\n";
            ++missing;
            continue;
        }
        if (files.zip.empty()) {
            db.import_csv(files.input, archive.source);
        } else {
            std::cerr << "Using daily SQLite archive: " << archive.date << '\n';
            db.import_sqlite(files.input, archive.source);
        }
        ++imported;
        if (!keep_downloads) {
            fs::remove(files.input);
            if (!files.zip.empty()) fs::remove(files.zip);
        }
    }
    std::cerr << "Daily archives: imported=" << imported << " skipped=" << skipped
              << " missing=" << missing << '\n';
    return missing == 0 ? 0 : 2;
}
