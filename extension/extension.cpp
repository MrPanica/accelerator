/*
 * =============================================================================
 * Accelerator Extension
 * Copyright (C) 2011 Asher Baker (asherkin).  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "extension.h"

#ifndef PLATFORM_ARCH_FOLDER
#define PLATFORM_ARCH_FOLDER ""
#endif

#include <sp_vm_api.h>

#include <IWebternet.h>
#include "MemoryDownloader.h"
#include "forwards.h"
#include "natives.h"

#if defined _LINUX
#include "client/linux/handler/exception_handler.h"
#include "common/linux/linux_libc_support.h"
#include "third_party/lss/linux_syscall_support.h"
#include "common/linux/dump_symbols.h"
#include "common/path_helper.h"

#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <paths.h>
#include <sys/wait.h>

class StderrInhibitor
{
	FILE *saved_stderr = nullptr;

public:
	StderrInhibitor() {
		saved_stderr = fdopen(dup(fileno(stderr)), "w");
		if (freopen(_PATH_DEVNULL, "w", stderr)) {
			// If it fails, not a lot we can (or should) do.
			// Add this brace section to silence gcc warnings.
		}
	}

	~StderrInhibitor() {
		fflush(stderr);
		dup2(fileno(saved_stderr), fileno(stderr));
		fclose(saved_stderr);
	}
};

// Taken from https://hg.mozilla.org/mozilla-central/file/3eb7623b5e63b37823d5e9c562d56e586604c823/build/unix/stdc%2B%2Bcompat/stdc%2B%2Bcompat.cpp
extern "C" void __attribute__((weak)) __cxa_throw_bad_array_new_length() {
	abort();
}

namespace std {
	/* We shouldn't be throwing exceptions at all, but it sadly turns out
	   we call STL (inline) functions that do. */
	void __attribute__((weak)) __throw_out_of_range_fmt(char const* fmt, ...) {
		va_list ap;
		char buf[1024];  // That should be big enough.

		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		buf[sizeof(buf) - 1] = 0;
		va_end(ap);

		__throw_range_error(buf);
	}
} // namespace std

// Updated versions of the SM ones for C++14
void operator delete(void *ptr, size_t sz) {
	free(ptr);
}

void operator delete[](void *ptr, size_t sz) {
	free(ptr);
}

#elif defined _WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _STDINT // ~.~
#include "client/windows/handler/exception_handler.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>

class StderrInhibitor
{
	int saved_stderr = -1;
	int null_fd = -1;

public:
	StderrInhibitor() {
		saved_stderr = _dup(_fileno(stderr));
		null_fd = _open("NUL", _O_WRONLY);
		if (saved_stderr != -1 && null_fd != -1) {
			_dup2(null_fd, _fileno(stderr));
		}
	}

	~StderrInhibitor() {
		fflush(stderr);
		if (saved_stderr != -1) {
			_dup2(saved_stderr, _fileno(stderr));
			_close(saved_stderr);
		}
		if (null_fd != -1) {
			_close(null_fd);
		}
	}
};

#else
#error Bad platform.
#endif

#include <google_breakpad/processor/minidump.h>
#include <google_breakpad/processor/minidump_processor.h>
#include <google_breakpad/processor/process_state.h>
#include <google_breakpad/processor/call_stack.h>
#include <google_breakpad/processor/stack_frame.h>
#include <google_breakpad/processor/stack_frame_cpu.h>
#include <google_breakpad/processor/basic_source_line_resolver.h>
#include <processor/pathname_stripper.h>
#include <processor/simple_symbol_supplier.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <streambuf>
#include <fstream>
#include <memory>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <cstdarg>
#include <ctime>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

Accelerator g_accelerator;
SMEXT_LINK(&g_accelerator);

IWebternet *webternet;
IGameConfig *gameconfig;

typedef void (*GetSpew_t)(char *buffer, size_t length);
GetSpew_t GetSpew;
#if defined _WINDOWS
typedef void(__fastcall *GetSpewFastcall_t)(char *buffer, size_t length);
GetSpewFastcall_t GetSpewFastcall;
#endif

char spewBuffer[65536]; // Hi.

char crashMap[256];
char crashGamePath[512];
char crashCommandLine[1024];
char crashSourceModPath[512];
char crashGameDirectory[256];
char crashSourceModVersion[32];
char steamInf[1024];

char dumpStoragePath[512];
char logPath[512];

google_breakpad::ExceptionHandler *handler = NULL;
static std::mutex acceleratorLogMutex;
static std::atomic<bool> acceleratorDebugLoggingEnabled{false};
static std::atomic<bool> acceleratorBackgroundThreadsStarted{false};

std::string ToLowerCopy(const std::string &value)
{
	std::string lowered(value);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return lowered;
}

static bool IsTruthyCoreConfigValue(const char *key, bool defaultValue)
{
	const char *raw = g_pSM->GetCoreConfigValue(key);
	if (!raw || !raw[0]) {
		return defaultValue;
	}

	std::string lowered = ToLowerCopy(raw);
	if (lowered == "1" || lowered == "y" || lowered == "yes" || lowered == "true" || lowered == "on") {
		return true;
	}
	if (lowered == "0" || lowered == "n" || lowered == "no" || lowered == "false" || lowered == "off") {
		return false;
	}
	return defaultValue;
}

static std::string RedactUrlForLog(const char *url)
{
	if (!url || !url[0]) {
		return "(not configured)";
	}

	std::string safeUrl(url);
	size_t query = safeUrl.find('?');
	if (query != std::string::npos) {
		safeUrl.erase(query);
		safeUrl += "?...";
	}

	return safeUrl;
}

static std::tm GetLocalLogTimestamp(std::time_t rawTime)
{
	std::tm localTime{};
#if defined _WINDOWS
	localtime_s(&localTime, &rawTime);
#else
	localtime_r(&rawTime, &localTime);
#endif
	return localTime;
}

static void AcceleratorWriteLogLine(const char *message)
{
	std::lock_guard<std::mutex> lock(acceleratorLogMutex);
	FILE *log = fopen(logPath, "a");
	if (!log) {
		return;
	}

	auto now = std::chrono::system_clock::now();
	auto rawTime = std::chrono::system_clock::to_time_t(now);
	std::tm localTime = GetLocalLogTimestamp(rawTime);
	char timestamp[32];
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &localTime);

	fprintf(log, "[%s] %s\n", timestamp, message);
	fflush(log);
	fclose(log);
}

static void AcceleratorLogMessageV(bool toConsole, bool debugOnly, const char *fmt, va_list ap)
{
	char message[1024];
	vsnprintf(message, sizeof(message), fmt, ap);
	message[sizeof(message) - 1] = '\0';

	if (debugOnly && !acceleratorDebugLoggingEnabled.load(std::memory_order_relaxed)) {
		return;
	}

	if (toConsole) {
		fprintf(stderr, "[Accelerator] %s\n", message);
		fflush(stderr);
	}

	AcceleratorWriteLogLine(message);
}

static void AcceleratorConsoleMessageV(const char *fmt, va_list ap)
{
	char message[1024];
	vsnprintf(message, sizeof(message), fmt, ap);
	message[sizeof(message) - 1] = '\0';

	fprintf(stderr, "[Accelerator] %s\n", message);
	fflush(stderr);
}

static void AcceleratorConsoleMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	AcceleratorConsoleMessageV(fmt, ap);
	va_end(ap);
}

static void AcceleratorConsoleWarning(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	AcceleratorLogMessageV(true, false, fmt, ap);
	va_end(ap);
}

static void AcceleratorDebugLog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	AcceleratorLogMessageV(false, true, fmt, ap);
	va_end(ap);
}

namespace
{
	using nlohmann::json;
	using google_breakpad::BasicSourceLineResolver;
	using google_breakpad::CallStack;
	using google_breakpad::MemoryRegion;
	using google_breakpad::Minidump;
	using google_breakpad::MinidumpMemoryList;
	using google_breakpad::MinidumpMemoryRegion;
	using google_breakpad::MinidumpProcessor;
	using google_breakpad::PathnameStripper;
	using google_breakpad::ProcessResult;
	using google_breakpad::ProcessState;
	using google_breakpad::SimpleSymbolSupplier;
	using google_breakpad::StackFrame;
	using google_breakpad::StackFrameAMD64;
	using google_breakpad::StackFrameX86;

	struct PendingDumpEntry
	{
		std::string name;
		std::string dumpPath;
		std::string metadataPath;
		bool hasMetadata = false;
	};

	struct LoadedDump
	{
		Minidump minidump;
		ProcessState processState;

		explicit LoadedDump(const std::string &path)
			: minidump(path)
		{
		}
	};

	size_t HexWidthForAddress(uint64_t value);
	std::string CollectHexDump(uint64_t base, const std::vector<uint8_t> &memory, const std::string &indent);
	const char *FrameTrustName(StackFrame::FrameTrust trust);

	enum class AcceleratorMode
	{
		Site,
		Local,
	};

	enum class LocalDumpJobState
	{
		Queued,
		Running,
		Done,
		Failed,
	};

	struct LocalDumpSettings
	{
		std::string gamePath;
		std::string sourceModPath;
		std::string carburetorPath;
		std::vector<std::string> symbolPaths;
		std::string localSymbolStoreRoot;
		std::string localOutputRoot;
#if defined _WINDOWS
		std::string dumpSymsPath;
#endif
	};

	struct LocalDumpJob
	{
		int id = 0;
		bool stackOnly = false;
		std::string dumpName;
		std::string mode;
		std::string requestedOutputPath;
		PendingDumpEntry entry;
		LocalDumpSettings settings;
		LocalDumpJobState state = LocalDumpJobState::Queued;
		std::string status;
		std::string outputPath;
		std::string result;
		std::string error;
		std::chrono::system_clock::time_point createdAt;
		std::chrono::system_clock::time_point finishedAt;
	};

	bool HasSuffix(const std::string &value, const char *suffix)
	{
		size_t suffixLength = strlen(suffix);
		return value.size() >= suffixLength && value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
	}

	bool ShouldDeleteProcessedDump()
	{
		return IsTruthyCoreConfigValue("MinidumpDeleteAfterProcessing", true);
	}

	AcceleratorMode GetAcceleratorMode()
	{
		const char *raw = g_pSM->GetCoreConfigValue("MinidumpMode");
		if (!raw || !raw[0]) {
			return AcceleratorMode::Site;
		}

		std::string lowered = ToLowerCopy(raw);
		if (lowered == "local") {
			return AcceleratorMode::Local;
		}
		return AcceleratorMode::Site;
	}

	bool IsLocalMode()
	{
		return GetAcceleratorMode() == AcceleratorMode::Local;
	}

	bool CollectPendingDumps(std::vector<PendingDumpEntry> &entries, std::string *error = nullptr)
	{
		entries.clear();

		IDirectory *dumps = libsys->OpenDirectory(dumpStoragePath);
		if (!dumps) {
			if (error) {
				*error = "Failed to open dump directory";
			}
			return false;
		}

		while (dumps->MoreFiles()) {
			if (!dumps->IsEntryFile()) {
				dumps->NextEntry();
				continue;
			}

			const char *entryName = dumps->GetEntryName();
			std::string fileName(entryName ? entryName : "");
			if (!HasSuffix(fileName, ".dmp")) {
				dumps->NextEntry();
				continue;
			}

			PendingDumpEntry entry;
			entry.name = fileName.substr(0, fileName.size() - 4);
			entry.dumpPath = std::string(dumpStoragePath) + "/" + fileName;
			entry.metadataPath = entry.dumpPath + ".txt";
			entry.hasMetadata = libsys->PathExists(entry.metadataPath.c_str());
			entries.push_back(entry);
			dumps->NextEntry();
		}

		libsys->CloseDirectory(dumps);
		std::sort(entries.begin(), entries.end(), [](const PendingDumpEntry &left, const PendingDumpEntry &right) {
			return left.name < right.name;
		});
		return true;
	}

	bool FindPendingDumpByName(const std::string &name, PendingDumpEntry &result, std::string *error = nullptr)
	{
		std::vector<PendingDumpEntry> entries;
		if (!CollectPendingDumps(entries, error)) {
			return false;
		}

		for (const PendingDumpEntry &entry : entries) {
			if (entry.name == name) {
				result = entry;
				return true;
			}
		}

		if (error) {
			*error = "Dump not found";
		}
		return false;
	}

	std::string ReadWholeFile(const std::string &path)
	{
		std::ifstream input(path, std::ios::in | std::ios::binary);
		if (!input) {
			return "";
		}

		std::ostringstream buffer;
		buffer << input.rdbuf();
		return buffer.str();
	}

	bool WriteWholeFile(const std::string &path, const std::string &contents, std::string *error)
	{
		std::ofstream output(path, std::ios::out | std::ios::binary | std::ios::trunc);
		if (!output) {
			if (error) {
				*error = "Failed to open output file for writing";
			}
			return false;
		}

		output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		if (!output.good()) {
			if (error) {
				*error = "Failed while writing the output file";
			}
			return false;
		}

		return true;
	}

	bool IsAbsolutePath(const std::string &path)
	{
		if (path.size() >= 1 && (path[0] == '/' || path[0] == '\\')) {
			return true;
		}
		return path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
			(path[2] == '/' || path[2] == '\\');
	}

	std::string TrimCopy(const std::string &value)
	{
		size_t start = 0;
		while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
			start++;
		}

		size_t end = value.size();
		while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
			end--;
		}

		return value.substr(start, end - start);
	}

	std::vector<std::string> SplitConfigPaths(const char *raw)
	{
		std::vector<std::string> paths;
		if (!raw || !raw[0]) {
			return paths;
		}

		std::string current;
		for (const char *cursor = raw; ; ++cursor) {
			char ch = *cursor;
			if (ch == '\0' || ch == ';' || ch == ',' || ch == '\n' || ch == '\r') {
				std::string trimmed = TrimCopy(current);
				if (!trimmed.empty()) {
					paths.push_back(trimmed);
				}
				current.clear();
				if (ch == '\0') {
					break;
				}
				continue;
			}
			current.push_back(ch);
		}

		return paths;
	}

	std::string JoinStrings(const std::vector<std::string> &items, const char *separator)
	{
		std::ostringstream out;
		for (size_t i = 0; i < items.size(); ++i) {
			if (i != 0) {
				out << separator;
			}
			out << items[i];
		}
		return out.str();
	}

	std::string StripLeadingNonJson(const std::string &raw);

	std::string EscapeJson(const std::string &input)
	{
		std::ostringstream out;
		for (unsigned char ch : input) {
			switch (ch) {
				case '\\': out << "\\\\"; break;
				case '"': out << "\\\""; break;
				case '\b': out << "\\b"; break;
				case '\f': out << "\\f"; break;
				case '\n': out << "\\n"; break;
				case '\r': out << "\\r"; break;
				case '\t': out << "\\t"; break;
				default:
					if (ch < 0x20) {
						out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec << std::setfill(' ');
					} else {
						out << static_cast<char>(ch);
					}
			}
		}
		return out.str();
	}

	std::string QuoteCommandArg(const std::string &value)
	{
#if defined _WINDOWS
		std::string normalized = value;
		std::replace(normalized.begin(), normalized.end(), '/', '\\');
#else
		const std::string &normalized = value;
#endif

		std::string escaped;
		escaped.reserve(normalized.size() + 2);
		escaped.push_back('"');
		for (char ch : normalized) {
			if (ch == '"') {
				escaped += "\\\"";
			} else {
				escaped.push_back(ch);
			}
		}
		escaped.push_back('"');
		return escaped;
	}

#if defined _WINDOWS
	std::wstring Utf8ToWide(const std::string &value)
	{
		if (value.empty()) {
			return std::wstring();
		}

		int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
		if (required <= 0) {
			return std::wstring();
		}

		std::vector<wchar_t> buffer(static_cast<size_t>(required), L'\0');
		if (MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, buffer.data(), required) <= 0) {
			return std::wstring();
		}

		return std::wstring(buffer.data());
	}

	std::string WideToUtf8(const std::wstring &value)
	{
		if (value.empty()) {
			return std::string();
		}

		int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (required <= 0) {
			return std::string();
		}

		std::vector<char> buffer(static_cast<size_t>(required), '\0');
		if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, buffer.data(), required, nullptr, nullptr) <= 0) {
			return std::string();
		}

		return std::string(buffer.data());
	}

	std::wstring GetDirectoryName(const std::wstring &path)
	{
		size_t slash = path.find_last_of(L"\\/");
		if (slash == std::wstring::npos) {
			return std::wstring();
		}

		return path.substr(0, slash);
	}

	std::wstring BuildWindowsCommandLine(const std::vector<std::string> &arguments)
	{
		std::ostringstream out;
		for (size_t i = 0; i < arguments.size(); ++i) {
			if (i != 0) {
				out << ' ';
			}
			out << QuoteCommandArg(arguments[i]);
		}
		return Utf8ToWide(out.str());
	}

#endif

	bool PathExistsFile(const std::string &path)
	{
		std::error_code ec;
		return std::filesystem::exists(std::filesystem::u8path(path), ec);
	}

	std::string NormalizePathString(const std::filesystem::path &path)
	{
		return path.lexically_normal().make_preferred().u8string();
	}

	bool StartsWithPathPrefix(const std::string &path, const char *prefix)
	{
		const size_t prefixLength = strlen(prefix);
		if (path.size() < prefixLength) {
			return false;
		}
		for (size_t i = 0; i < prefixLength; ++i) {
			char left = static_cast<char>(std::tolower(static_cast<unsigned char>(path[i])));
			char right = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
			if (left != right) {
				return false;
			}
		}
		return true;
	}

	std::string ResolveSourceModRelativePath(const std::string &configured,
		const std::string &sourceModRoot,
		const std::string &gameRoot)
	{
		if (configured.empty()) {
			return "";
		}
		if (IsAbsolutePath(configured)) {
			return NormalizePathString(std::filesystem::u8path(configured));
		}

		std::string normalized = configured;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (StartsWithPathPrefix(normalized, "addons/sourcemod/")) {
			return NormalizePathString(std::filesystem::u8path(gameRoot) / std::filesystem::u8path(normalized));
		}
		if (ToLowerCopy(normalized) == "addons/sourcemod") {
			return NormalizePathString(std::filesystem::u8path(gameRoot) / "addons" / "sourcemod");
		}
		return NormalizePathString(std::filesystem::u8path(sourceModRoot) / std::filesystem::u8path(normalized));
	}

	std::string GetDefaultCarburetorPath(const std::string &sourceModRoot)
	{
#if defined _WINDOWS
		return NormalizePathString(std::filesystem::u8path(sourceModRoot) / "bin" / (std::string(PLATFORM_ARCH_FOLDER) + "carburetor.exe"));
#else
		return NormalizePathString(std::filesystem::u8path(sourceModRoot) / "bin" / (std::string(PLATFORM_ARCH_FOLDER) + "carburetor"));
#endif
	}

	std::string GetConfiguredCarburetorPath()
	{
		const char *configured = g_pSM->GetCoreConfigValue("MinidumpLocalCarburetorPath");
		if (configured && configured[0]) {
			return ResolveSourceModRelativePath(configured, crashSourceModPath, crashGamePath);
		}
		return GetDefaultCarburetorPath(crashSourceModPath);
	}

	std::string GetLocalSymbolStoreRoot()
	{
		return NormalizePathString(std::filesystem::u8path(crashSourceModPath) / "data" / "dumps" / "symbols");
	}

	std::string GetLocalOutputRoot()
	{
		return NormalizePathString(std::filesystem::u8path(crashSourceModPath) / "data" / "dumps" / "outputs");
	}

	std::vector<std::string> GetConfiguredLocalSymbolPaths()
	{
		std::vector<std::string> results;
		auto appendIfUniqueExisting = [&](const std::string &path) {
			if (path.empty() || !PathExistsFile(path)) {
				return;
			}
			if (std::find(results.begin(), results.end(), path) == results.end()) {
				results.push_back(path);
			}
		};

		for (const std::string &item : SplitConfigPaths(g_pSM->GetCoreConfigValue("MinidumpLocalSymbolPath"))) {
			appendIfUniqueExisting(ResolveSourceModRelativePath(item, crashSourceModPath, crashGamePath));
		}

		appendIfUniqueExisting(GetLocalSymbolStoreRoot());

		return results;
	}

	bool EnsureDirectoryExists(const std::string &path, std::string &error)
	{
		try {
			std::filesystem::create_directories(path);
			return true;
		} catch (const std::exception &ex) {
			error = ex.what();
			return false;
		}
	}

	bool RunCommandCapture(const std::string &commandLine, std::string &output, int &exitCode, std::string &error)
	{
		output.clear();
		exitCode = -1;

#if defined _WINDOWS
		std::string wrappedCommand = commandLine + " 2>&1";
		FILE *pipe = _popen(wrappedCommand.c_str(), "r");
#else
		FILE *pipe = popen((commandLine + " 2>&1").c_str(), "r");
#endif
		if (!pipe) {
			error = "Failed to launch external command";
			return false;
		}

		char buffer[4096];
		while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
			output += buffer;
		}

#if defined _WINDOWS
		exitCode = _pclose(pipe);
#else
		exitCode = pclose(pipe);
		if (WIFEXITED(exitCode)) {
			exitCode = WEXITSTATUS(exitCode);
		}
#endif

		return true;
	}

#if defined _WINDOWS
	bool RunProcessCapture(const std::vector<std::string> &arguments,
		std::string &stdoutOutput,
		std::string &stderrOutput,
		int &exitCode,
		std::string &error)
	{
		stdoutOutput.clear();
		stderrOutput.clear();
		exitCode = -1;

		if (arguments.empty()) {
			error = "No command arguments were provided";
			return false;
		}

		std::wstring application = Utf8ToWide(arguments[0]);
		if (application.empty()) {
			error = "Failed to convert executable path to UTF-16";
			return false;
		}

		std::wstring commandLine = BuildWindowsCommandLine(arguments);
		if (commandLine.empty()) {
			error = "Failed to build command line";
			return false;
		}

		SECURITY_ATTRIBUTES securityAttributes{};
		securityAttributes.nLength = sizeof(securityAttributes);
		securityAttributes.bInheritHandle = TRUE;

		HANDLE outputReadPipe = nullptr;
		HANDLE outputWritePipe = nullptr;
		if (!CreatePipe(&outputReadPipe, &outputWritePipe, &securityAttributes, 0)) {
			error = "CreatePipe failed";
			return false;
		}

		if (!SetHandleInformation(outputReadPipe, HANDLE_FLAG_INHERIT, 0)) {
			CloseHandle(outputReadPipe);
			CloseHandle(outputWritePipe);
			error = "SetHandleInformation failed";
			return false;
		}

		STARTUPINFOW startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		startupInfo.dwFlags = STARTF_USESTDHANDLES;
		startupInfo.hStdInput = nullptr;
		startupInfo.hStdOutput = outputWritePipe;
		startupInfo.hStdError = outputWritePipe;

		PROCESS_INFORMATION processInfo{};
		std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
		mutableCommandLine.push_back(L'\0');
		std::wstring workingDirectory = GetDirectoryName(application);

		BOOL created = CreateProcessW(
			application.c_str(),
			mutableCommandLine.data(),
			nullptr,
			nullptr,
			TRUE,
			CREATE_NO_WINDOW,
			nullptr,
			workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
			&startupInfo,
			&processInfo
		);

		CloseHandle(outputWritePipe);

		if (!created) {
			DWORD lastError = GetLastError();
			CloseHandle(outputReadPipe);

			LPWSTR messageBuffer = nullptr;
			FormatMessageW(
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr,
				lastError,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				reinterpret_cast<LPWSTR>(&messageBuffer),
				0,
				nullptr
			);

			std::wstring wideError = messageBuffer ? messageBuffer : L"CreateProcessW failed";
			if (messageBuffer) {
				LocalFree(messageBuffer);
			}

			error = WideToUtf8(wideError);
			return false;
		}

		char buffer[4096];
		DWORD bytesRead = 0;
		while (ReadFile(outputReadPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead != 0) {
			stdoutOutput.append(buffer, buffer + bytesRead);
		}
		CloseHandle(outputReadPipe);

		WaitForSingleObject(processInfo.hProcess, INFINITE);
		DWORD processExitCode = 0;
		if (GetExitCodeProcess(processInfo.hProcess, &processExitCode)) {
			exitCode = static_cast<int>(processExitCode);
		}

		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);

		return true;
	}
#endif

	#if defined _WINDOWS
	std::string GetBundledDumpSymsPath(const LocalDumpSettings &settings)
	{
		return settings.dumpSymsPath;
	}
	#endif

	bool TryRunCarburetorRaw(const PendingDumpEntry &entry,
		const LocalDumpSettings &settings,
		std::string &output,
		std::string &error)
	{
		const std::string &carburetorPath = settings.carburetorPath;
		if (carburetorPath.empty() || !PathExistsFile(carburetorPath)) {
			error = "Carburetor binary was not found. Configure MinidumpLocalCarburetorPath or place carburetor in addons/sourcemod/bin.";
			return false;
		}

		const std::vector<std::string> &symbolPaths = settings.symbolPaths;
		std::vector<std::string> arguments{carburetorPath, entry.dumpPath};
		for (const std::string &symbolPath : symbolPaths) {
			arguments.push_back(symbolPath);
		}

		int exitCode = -1;
#if defined _WINDOWS
		std::string stderrOutput;
		if (!RunProcessCapture(arguments, output, stderrOutput, exitCode, error)) {
			return false;
		}
		output = StripLeadingNonJson(output);
		if (output.empty() && !stderrOutput.empty()) {
			error = stderrOutput;
			return false;
		}
#else
		std::ostringstream commandLine;
		commandLine << QuoteCommandArg(carburetorPath) << " " << QuoteCommandArg(entry.dumpPath);
		for (const std::string &symbolPath : symbolPaths) {
			commandLine << " " << QuoteCommandArg(symbolPath);
		}
		if (!RunCommandCapture(commandLine.str(), output, exitCode, error)) {
			return false;
		}
#endif
		if (exitCode != 0) {
			std::ostringstream out;
			out << "Carburetor exited with code " << exitCode;
			if (!output.empty()) {
				out << ": " << output;
			}
			error = out.str();
			return false;
		}

		return true;
	}

	std::string SymbolLeafNameForModule(const std::string &debugFileName)
	{
		std::string lowered = ToLowerCopy(debugFileName);
		if (lowered.size() > 4 && lowered.substr(lowered.size() - 4) == ".pdb") {
			return debugFileName.substr(0, debugFileName.size() - 4) + ".sym";
		}
		return debugFileName + ".sym";
	}

#if defined _WINDOWS
	const char *kMissingAdjacentPdb = "No adjacent PDB found";

	std::string GetAdjacentPdbPath(const std::string &modulePath)
	{
		if (modulePath.empty()) {
			return "";
		}

		size_t slash = modulePath.find_last_of("/\\");
		size_t dot = modulePath.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
			return modulePath + ".pdb";
		}
		return modulePath.substr(0, dot) + ".pdb";
	}
#endif

	bool LooksLikeSymbolizableModulePath(const std::string &path)
	{
		if (path.empty() || !IsAbsolutePath(path)) {
			return false;
		}

		const std::string lowered = ToLowerCopy(path);
#if defined _WINDOWS
		if (HasSuffix(lowered, ".dll") || HasSuffix(lowered, ".exe") || HasSuffix(lowered, ".pdb")) {
			return true;
		}
		return false;
#else
		if (lowered.find("/dev/shm/") == 0) {
			return false;
		}
		if (lowered.find(".mmdb") != std::string::npos) {
			return false;
		}
		if (HasSuffix(lowered, ".vpk") || HasSuffix(lowered, ".txt") || HasSuffix(lowered, ".cfg")) {
			return false;
		}
		if (HasSuffix(lowered, ".so") || HasSuffix(lowered, ".srv.so") || HasSuffix(lowered, ".dll")) {
			return true;
		}
		if (lowered.find("/bin/srcds_linux") != std::string::npos || HasSuffix(lowered, "/srcds_linux")) {
			return true;
		}
		return false;
#endif
	}

	bool GenerateLocalSymbolFile(const google_breakpad::CodeModule *module,
		const LocalDumpSettings &settings,
		std::string &storedPath,
		std::string &error)
	{
		storedPath.clear();
		if (!module) {
			error = "Module is null";
			return false;
		}

		std::string debugFile = module->debug_file();
		if (debugFile.empty() || !IsAbsolutePath(debugFile)) {
			debugFile = module->code_file();
		}
		if (debugFile.empty() || !IsAbsolutePath(debugFile)) {
			error = "Module path is not absolute";
			return false;
		}
		if (!LooksLikeSymbolizableModulePath(debugFile)) {
			error = "Module is not a supported symbolizable binary";
			return false;
		}

		std::string debugFileName = PathnameStripper::File(module->debug_file());
		if (debugFileName.empty()) {
			debugFileName = PathnameStripper::File(module->code_file());
		}
		if (debugFileName.empty()) {
			error = "Module debug file name is empty";
			return false;
		}

		const std::string identifier = module->debug_identifier();
		if (identifier.empty()) {
			error = "Module debug identifier is empty";
			return false;
		}

		const std::string storeRoot = settings.localSymbolStoreRoot;
		const std::string moduleDir = storeRoot + "/" + debugFileName + "/" + identifier;
		if (!EnsureDirectoryExists(moduleDir, error)) {
			return false;
		}

		storedPath = moduleDir + "/" + SymbolLeafNameForModule(debugFileName);
		if (PathExistsFile(storedPath)) {
			return true;
		}

#if defined _WINDOWS
		const std::string moduleBinary = module->code_file();
		if (moduleBinary.empty() || !IsAbsolutePath(moduleBinary)) {
			error = "Module binary path is not absolute";
			return false;
		}

		const std::string pdbPath = GetAdjacentPdbPath(moduleBinary);
		if (pdbPath.empty() || !PathExistsFile(pdbPath)) {
			error = kMissingAdjacentPdb;
			return false;
		}

		const std::string dumpSymsPath = GetBundledDumpSymsPath(settings);
		if (dumpSymsPath.empty() || !PathExistsFile(dumpSymsPath)) {
			error = "dump_syms.exe was not found in addons/sourcemod/bin";
			return false;
		}

		std::string outputText;
		std::string errorText;
		int exitCode = -1;
		if (!RunProcessCapture({dumpSymsPath, debugFile}, outputText, errorText, exitCode, error)) {
			return false;
		}
		if (exitCode != 0) {
			std::ostringstream out;
			out << "dump_syms.exe exited with code " << exitCode;
			if (!errorText.empty()) {
				out << ": " << errorText;
			} else if (!outputText.empty()) {
				out << ": " << outputText;
			}
			error = out.str();
			return false;
		}
		if (outputText.empty()) {
			error = "dump_syms.exe returned an empty symbol file";
			return false;
		}

		std::string writeError;
		if (!WriteWholeFile(storedPath, outputText, &writeError)) {
			error = writeError;
			return false;
		}

		return true;
#else
		auto debugFileDir = google_breakpad::DirName(debugFile);
		std::vector<std::string> debugDirs{
			debugFileDir,
			debugFileDir + "/.debug",
			"/usr/lib/debug" + debugFileDir,
		};

		std::ostringstream outputStream;
		google_breakpad::DumpOptions options(ALL_SYMBOL_DATA, true, true, false);
		{
			StderrInhibitor stderrInhibitor;
			if (!WriteSymbolFile(debugFile, debugFile, "Linux", "", debugDirs, options, outputStream)) {
				outputStream.str("");
				outputStream.clear();
				if (!WriteSymbolFile(debugFile, debugFile, "Linux", "", {}, options, outputStream)) {
					error = "WriteSymbolFile failed";
					return false;
				}
			}
		}

		std::string writeError;
		if (!WriteWholeFile(storedPath, outputStream.str(), &writeError)) {
			error = writeError;
			return false;
		}

		return true;
#endif
	}

	const CallStack *GetRequestingThreadStack(const ProcessState &processState, int &threadIndex);

	std::set<std::string> CollectRequestingThreadModulePaths(const LoadedDump &loadedDump)
	{
		std::set<std::string> modulePaths;
		int threadIndex = 0;
		const CallStack *stack = GetRequestingThreadStack(loadedDump.processState, threadIndex);
		if (!stack) {
			return modulePaths;
		}

		const auto *frames = stack->frames();
		for (size_t i = 0; i < frames->size(); ++i) {
			const StackFrame *frame = frames->at(i);
			if (!frame || !frame->module) {
				continue;
			}

			std::string path = frame->module->debug_file();
			if (path.empty() || !IsAbsolutePath(path)) {
				path = frame->module->code_file();
			}
			if (LooksLikeSymbolizableModulePath(path)) {
				modulePaths.insert(path);
			}
		}

		return modulePaths;
	}

	void EnsureLocalSymbolsForDump(const LoadedDump &loadedDump, const LocalDumpSettings &settings)
	{
		const auto *modules = loadedDump.processState.modules();
		if (!modules) {
			return;
		}

		std::string rootError;
		if (!EnsureDirectoryExists(settings.localSymbolStoreRoot, rootError)) {
			AcceleratorConsoleWarning("Failed to create local symbol store: %s", rootError.c_str());
			return;
		}

		bool hadVerboseFailure = false;
		std::set<std::string> requestedPaths = CollectRequestingThreadModulePaths(loadedDump);
		for (unsigned int i = 0; i < modules->module_count(); ++i) {
			const google_breakpad::CodeModule *module = modules->GetModuleAtIndex(i);
			if (!module) {
				continue;
			}

			std::string modulePath = module->debug_file();
			if (modulePath.empty() || !IsAbsolutePath(modulePath)) {
				modulePath = module->code_file();
			}
			if (!requestedPaths.empty() && requestedPaths.find(modulePath) == requestedPaths.end()) {
				continue;
			}

			std::string storedPath;
			std::string error;
			if (!GenerateLocalSymbolFile(module, settings, storedPath, error)) {
#if defined _WINDOWS
				if (error == kMissingAdjacentPdb) {
					continue;
				}
#endif
				hadVerboseFailure = hadVerboseFailure || !error.empty();
			}
		}

	}

	bool ParseCarburetorJson(const std::string &raw, json &document, std::string &error)
	{
		try {
			document = json::parse(StripLeadingNonJson(raw));
			if (!document.is_object()) {
				error = "Carburetor did not return a JSON object";
				return false;
			}
			return true;
		} catch (const std::exception &ex) {
			error = ex.what();
			return false;
		}
	}

	int DecodeBase64Value(unsigned char ch)
	{
		if (ch >= 'A' && ch <= 'Z') return ch - 'A';
		if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
		if (ch >= '0' && ch <= '9') return ch - '0' + 52;
		if (ch == '+') return 62;
		if (ch == '/') return 63;
		return -1;
	}

	bool DecodeBase64(const std::string &input, std::vector<uint8_t> &output)
	{
		output.clear();
		int val = 0;
		int valb = -8;
		for (unsigned char ch : input) {
			if (std::isspace(ch)) {
				continue;
			}
			if (ch == '=') {
				break;
			}
			int decoded = DecodeBase64Value(ch);
			if (decoded < 0) {
				return false;
			}
			val = (val << 6) + decoded;
			valb += 6;
			if (valb >= 0) {
				output.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
				valb -= 8;
			}
		}
		return true;
	}

	uint64_t JsonUInt64(const json &value, uint64_t fallback = 0)
	{
		if (value.is_number_unsigned()) {
			return value.get<uint64_t>();
		}
		if (value.is_number_integer()) {
			return static_cast<uint64_t>(value.get<int64_t>());
		}
		if (value.is_string()) {
			try {
				return std::stoull(value.get<std::string>());
			} catch (...) {
				return fallback;
			}
		}
		return fallback;
	}

	std::string JsonString(const json &value)
	{
		return value.is_string() ? value.get<std::string>() : "";
	}

	std::string RenderCarburetorInstructions(const json &instructionsValue, uint64_t ip, const std::string &indent)
	{
		if (!instructionsValue.is_array()) {
			return "";
		}

		std::vector<json> instructions;
		for (const auto &item : instructionsValue) {
			if (item.is_object()) {
				instructions.push_back(item);
			}
		}
		if (instructions.empty()) {
			return "";
		}

		int crashOpcode = -1;
		size_t bytesPerLine = 0;
		for (size_t i = 0; i < instructions.size(); ++i) {
			uint64_t currentOffset = JsonUInt64(instructions[i].value("offset", json()));
			uint64_t nextOffset = (i + 1 < instructions.size()) ? JsonUInt64(instructions[i + 1].value("offset", json())) : currentOffset;
			if (ip >= currentOffset && (i + 1 == instructions.size() || ip < nextOffset)) {
				crashOpcode = static_cast<int>(i);
				break;
			}
		}

		if (crashOpcode >= 0) {
			for (size_t i = 0; i < instructions.size(); ++i) {
				if (static_cast<int>(i) < crashOpcode - 5 || static_cast<int>(i) > crashOpcode + 5) {
					continue;
				}
				const std::string hex = JsonString(instructions[i].value("hex", json()));
				bytesPerLine = (std::max)(bytesPerLine, hex.size() / 2);
			}
		}

		std::ostringstream out;
		for (size_t i = 0; i < instructions.size(); ++i) {
			if (crashOpcode >= 0 && (static_cast<int>(i) < crashOpcode - 5 || static_cast<int>(i) > crashOpcode + 5)) {
				continue;
			}

			std::string line = indent;
			if (crashOpcode >= 0 && static_cast<int>(i) == crashOpcode && line.size() >= 3) {
				line = "  >" + line.substr(3);
			}

			const uint64_t offset = JsonUInt64(instructions[i].value("offset", json()));
			const std::string hex = JsonString(instructions[i].value("hex", json()));
			const std::string mnemonic = JsonString(instructions[i].value("mnemonic", json()));

			std::ostringstream hexFormatted;
			for (size_t index = 0; index < hex.size(); index += 2) {
				if (index > 0) {
					hexFormatted << ' ';
				}
				hexFormatted << hex.substr(index, (std::min<size_t>)(2, hex.size() - index));
			}

			out << line << std::hex << std::setw(8) << std::setfill('0') << offset << std::dec << std::setfill(' ')
				<< "  " << std::left << std::setw(static_cast<int>((bytesPerLine * 3) > 0 ? (bytesPerLine * 3) - 1 : 0))
				<< hexFormatted.str() << std::right << "  " << mnemonic << "\n";
		}

		return out.str();
	}

	std::string RenderCarburetorRegisters(const json &registersValue, const std::string &indent)
	{
		if (!registersValue.is_object()) {
			return "";
		}

		static const std::vector<std::string> preferredOrder = {
			"eip", "esp", "ebp", "ebx",
			"esi", "edi", "eax", "ecx",
			"edx", "efl",
			"rax", "rdx", "rcx", "rbx",
			"rsi", "rdi", "rbp", "rsp",
			"r8", "r9", "r10", "r11",
			"r12", "r13", "r14", "r15", "rip",
		};

		std::vector<std::pair<std::string, uint64_t>> registers;
		std::set<std::string> printed;

		for (const std::string &name : preferredOrder) {
			auto it = registersValue.find(name);
			if (it == registersValue.end()) {
				continue;
			}
			registers.emplace_back(name, JsonUInt64(*it));
			printed.insert(name);
		}

		for (auto it = registersValue.begin(); it != registersValue.end(); ++it) {
			if (printed.find(it.key()) != printed.end()) {
				continue;
			}
			registers.emplace_back(it.key(), JsonUInt64(it.value()));
		}

		if (registers.empty()) {
			return "";
		}

		size_t width = 8;
		for (const auto &entry : registers) {
			width = (std::max)(width, HexWidthForAddress(entry.second));
		}

		std::ostringstream out;
		size_t count = 0;
		out << indent;
		for (const auto &entry : registers) {
			out << entry.first << ": 0x" << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << entry.second << std::dec << std::setfill(' ');
			count++;
			if (count % 4 == 0) {
				out << "\n";
				if (count != registers.size()) {
					out << indent;
				}
			} else if (count != registers.size()) {
				out << "  ";
			}
		}
		if (count % 4 != 0) {
			out << "\n";
		}

		return out.str();
	}

	std::string RenderCarburetorStack(const json &document)
	{
		std::ostringstream out;

		if (document.value("crashed", false)) {
			out << JsonString(document.value("crash_reason", json())) << " accessing 0x"
				<< std::hex << JsonUInt64(document.value("crash_address", json())) << std::dec << "\n\n";
		}

		if (!document.contains("requesting_thread") || !document["requesting_thread"].is_number_integer()) {
			return out.str();
		}

		const int requestingThread = document["requesting_thread"].get<int>();
		if (!document.contains("threads") || !document["threads"].is_array() ||
			requestingThread < 0 || requestingThread >= static_cast<int>(document["threads"].size())) {
			return out.str();
		}

		const json &thread = document["threads"][requestingThread];
		if (!thread.is_array()) {
			return out.str();
		}

		out << "Thread " << requestingThread << " (crashed):\n";

		const size_t numFrames = thread.size();
		const size_t frameDigits = (std::max<size_t>)(1, std::to_string(numFrames > 0 ? numFrames - 1 : 0).size());

		for (size_t i = 0; i < numFrames; ++i) {
			const json &frame = thread[i];
			if (!frame.is_object()) {
				continue;
			}

			std::ostringstream prefix;
			prefix << std::setw(static_cast<int>(frameDigits)) << std::setfill(' ') << i << ": ";
			const std::string prefixText = prefix.str();
			const std::string indent = "  " + std::string(prefixText.size(), ' ');

			out << "  " << prefixText << JsonString(frame.value("rendered", json())) << "\n";

			if (frame.contains("url") && frame["url"].is_string()) {
				out << indent << frame["url"].get<std::string>() << "\n";
			}

			if (frame.contains("registers")) {
				out << RenderCarburetorRegisters(frame["registers"], indent);
				out << "\n";
			}

			if (frame.contains("instructions")) {
				out << RenderCarburetorInstructions(frame["instructions"], JsonUInt64(frame.value("instruction", json())), indent);
				out << "\n";
			}

			if (frame.contains("stack") && frame["stack"].is_string()) {
				std::vector<uint8_t> decoded;
				if (DecodeBase64(frame["stack"].get<std::string>(), decoded)) {
					uint64_t base = 0;
					if (frame.contains("registers") && frame["registers"].is_object()) {
						const json &registers = frame["registers"];
						if (registers.contains("esp")) {
							base = JsonUInt64(registers["esp"]);
						} else if (registers.contains("rsp")) {
							base = JsonUInt64(registers["rsp"]);
						}
					}
					out << CollectHexDump(base, decoded, indent);
					out << "\n";
				}
			}

			out << indent << "Found via " << FrameTrustName(static_cast<StackFrame::FrameTrust>(frame.value("trust", 0))) << "\n\n\n";
		}

		return out.str();
	}

	std::string RenderCarburetorMemory(const json &document, std::string &error)
	{
		if (!document.contains("memory") || !document["memory"].is_array()) {
			error = "No memory regions in carburetor JSON";
			return "";
		}

		std::vector<json> regions;
		for (const auto &region : document["memory"]) {
			if (region.is_object()) {
				regions.push_back(region);
			}
		}
		if (regions.empty()) {
			error = "No memory regions in carburetor JSON";
			return "";
		}

		std::sort(regions.begin(), regions.end(), [](const json &left, const json &right) {
			return JsonUInt64(left.value("base", json())) < JsonUInt64(right.value("base", json()));
		});

		const uint64_t start = JsonUInt64(regions.front().value("base", json()));
		const uint64_t end = JsonUInt64(regions.back().value("base", json())) + JsonUInt64(regions.back().value("size", json()));
		const uint64_t size = end > start ? (end - start) : 0;

		uint64_t real = 0;
		for (const auto &region : regions) {
			real += JsonUInt64(region.value("size", json()));
		}

		std::ostringstream out;
		const size_t addressWidth = HexWidthForAddress(end);
		out << "Got " << real << " bytes of memory covering "
			<< std::hex << std::setw(static_cast<int>(addressWidth)) << std::setfill('0') << start << " to "
			<< std::setw(static_cast<int>(addressWidth)) << end << std::dec << std::setfill(' ')
			<< " (" << (size > 0 ? (static_cast<double>(real) / static_cast<double>(size)) : 1.0) << "% coverage)\n\n";

		for (const auto &region : regions) {
			const uint64_t base = JsonUInt64(region.value("base", json()));
			const uint64_t declaredSize = JsonUInt64(region.value("size", json()));
			std::vector<uint8_t> decoded;
			if (!DecodeBase64(JsonString(region.value("data", json())), decoded)) {
				error = "Failed to decode base64 memory region";
				return "";
			}
			if (decoded.size() != declaredSize) {
				error = "Decoded memory region size mismatch";
				return "";
			}
			out << CollectHexDump(base, decoded, "");
			out << "\n";
		}

		return out.str();
	}

	size_t HexWidthForAddress(uint64_t value)
	{
		return value > 0xFFFFFFFFULL ? 16 : 8;
	}

	std::string RenderFrame(const StackFrame *frame)
	{
		std::ostringstream out;
		out << std::hex;
		if (frame->module) {
			out << PathnameStripper::File(frame->module->code_file());
			if (!frame->function_name.empty()) {
				out << "!" << frame->function_name;
				if (!frame->source_file_name.empty()) {
					out << " [ " << PathnameStripper::File(frame->source_file_name) << ":" << std::dec << frame->source_line
						<< std::hex << " + 0x" << (frame->ReturnAddress() - frame->source_line_base) << " ]";
				} else {
					out << " + 0x" << (frame->ReturnAddress() - frame->function_base);
				}
			} else {
				out << " + 0x" << (frame->ReturnAddress() - frame->module->base_address());
			}
		} else {
			out << "0x" << frame->ReturnAddress();
		}
		out << std::dec;
		return out.str();
	}

	std::optional<std::vector<uint8_t>> GetFrameStackContents(const StackFrame *frame,
		const StackFrame *prevFrame,
		const std::string &cpu,
		const MemoryRegion *memory)
	{
		if (!memory) {
			return {};
		}

		uint64_t stackBegin = 0;
		uint64_t stackEnd = 0;
		if (cpu == "x86") {
			const StackFrameX86 *frameX86 = static_cast<const StackFrameX86 *>(frame);
			const StackFrameX86 *prevFrameX86 = static_cast<const StackFrameX86 *>(prevFrame);
			if ((frameX86->context_validity & StackFrameX86::CONTEXT_VALID_ESP) &&
				(prevFrameX86->context_validity & StackFrameX86::CONTEXT_VALID_ESP)) {
				stackBegin = frameX86->context.esp;
				stackEnd = prevFrameX86->context.esp;
			}
		} else if (cpu == "amd64") {
			const StackFrameAMD64 *frameAMD64 = static_cast<const StackFrameAMD64 *>(frame);
			const StackFrameAMD64 *prevFrameAMD64 = static_cast<const StackFrameAMD64 *>(prevFrame);
			if ((frameAMD64->context_validity & StackFrameAMD64::CONTEXT_VALID_RSP) &&
				(prevFrameAMD64->context_validity & StackFrameAMD64::CONTEXT_VALID_RSP)) {
				stackBegin = frameAMD64->context.rsp;
				stackEnd = prevFrameAMD64->context.rsp;
			}
		}

		if (!stackBegin || !stackEnd || stackEnd <= stackBegin) {
			return {};
		}

		size_t stackSize = static_cast<size_t>(stackEnd - stackBegin);
		std::vector<uint8_t> bytes;
		bytes.reserve(stackSize);

		for (uint64_t address = stackBegin; address < stackEnd; ++address) {
			uint8_t value = 0;
			if (!memory->GetMemoryAtAddress(address, &value)) {
				return {};
			}
			bytes.push_back(value);
		}

		return bytes;
	}

	std::string CollectHexDump(uint64_t base, const std::vector<uint8_t> &memory, const std::string &indent)
	{
		std::ostringstream out;
		const size_t rowBytes = 16;
		const size_t chunkBytes = 8;
		const size_t addressWidth = HexWidthForAddress(base + memory.size());

		for (size_t offset = 0; offset < memory.size(); offset += rowBytes) {
			out << indent << std::hex << std::setw(static_cast<int>(addressWidth)) << std::setfill('0') << (base + offset) << "  ";

			std::string ascii;
			for (size_t i = 0; i < rowBytes; ++i) {
				if (offset + i < memory.size()) {
					unsigned byte = memory[offset + i];
					out << std::setw(2) << byte << " ";
					ascii.push_back((byte >= 32 && byte <= 126) ? static_cast<char>(byte) : '.');
				} else {
					out << "   ";
					ascii.push_back(' ');
				}

				if (i + 1 < rowBytes && ((i + 1) % chunkBytes) == 0) {
					out << ' ';
				}
			}

			out << " " << ascii << "\n";
		}

		out << std::dec << std::setfill(' ');
		return out.str();
	}

	void AppendRegisterDump(std::ostringstream &out, const StackFrame *frame, const std::string &indent)
	{
		if (!frame) {
			return;
		}

		auto appendLine = [&](const std::vector<std::pair<const char *, uint64_t>> &registers) {
			size_t count = 0;
			size_t registerWidth = 8;
			for (const auto &entry : registers) {
				registerWidth = (std::max)(registerWidth, HexWidthForAddress(entry.second));
			}
			out << indent;
			for (const auto &entry : registers) {
				out << entry.first << ": 0x" << std::hex << std::setw(static_cast<int>(registerWidth)) << std::setfill('0') << entry.second << std::dec << std::setfill(' ');
				count++;
				if (count % 4 == 0) {
					out << "\n";
					if (count != registers.size()) {
						out << indent;
					}
				} else if (count != registers.size()) {
					out << "  ";
				}
			}
			if (count % 4 != 0) {
				out << "\n";
			}
		};

		if (const auto *frameX86 = dynamic_cast<const StackFrameX86 *>(frame)) {
			std::vector<std::pair<const char *, uint64_t>> registers = {
				{"eip", frameX86->context.eip},
				{"esp", frameX86->context.esp},
				{"ebp", frameX86->context.ebp},
				{"ebx", frameX86->context.ebx},
				{"esi", frameX86->context.esi},
				{"edi", frameX86->context.edi},
				{"eax", frameX86->context.eax},
				{"ecx", frameX86->context.ecx},
				{"edx", frameX86->context.edx},
				{"efl", frameX86->context.eflags},
			};
			appendLine(registers);
		} else if (const auto *frameAMD64 = dynamic_cast<const StackFrameAMD64 *>(frame)) {
			std::vector<std::pair<const char *, uint64_t>> registers = {
				{"rip", frameAMD64->context.rip},
				{"rsp", frameAMD64->context.rsp},
				{"rbp", frameAMD64->context.rbp},
				{"rax", frameAMD64->context.rax},
				{"rbx", frameAMD64->context.rbx},
				{"rcx", frameAMD64->context.rcx},
				{"rdx", frameAMD64->context.rdx},
				{"rsi", frameAMD64->context.rsi},
				{"rdi", frameAMD64->context.rdi},
				{"r8", frameAMD64->context.r8},
				{"r9", frameAMD64->context.r9},
				{"r10", frameAMD64->context.r10},
				{"r11", frameAMD64->context.r11},
				{"r12", frameAMD64->context.r12},
				{"r13", frameAMD64->context.r13},
				{"r14", frameAMD64->context.r14},
				{"r15", frameAMD64->context.r15},
			};
			appendLine(registers);
		}
	}

	const char *FrameTrustName(StackFrame::FrameTrust trust)
	{
		static const char *names[] = {
			"unknown",
			"stack scanning",
			"call frame info with scanning",
			"previous frame's frame pointer",
			"call frame info",
			"external stack walker",
			"instruction pointer in context",
		};

		size_t index = static_cast<size_t>(trust);
		if (index >= (sizeof(names) / sizeof(names[0]))) {
			return names[0];
		}
		return names[index];
	}

	bool LoadDump(LoadedDump &loadedDump, std::string &error)
	{
		{
			StderrInhibitor stderrInhibitor;
			if (!loadedDump.minidump.Read()) {
				error = "Minidump could not be read";
				return false;
			}

			MinidumpProcessor processor(nullptr, nullptr);
			ProcessResult result = processor.Process(&loadedDump.minidump, &loadedDump.processState);
			if (result != google_breakpad::PROCESS_OK) {
				std::ostringstream out;
				out << "MinidumpProcessor failed with status " << result;
				error = out.str();
				return false;
			}
		}

		return true;
	}

	bool ReloadDumpWithLocalSymbols(LoadedDump &loadedDump,
		const std::vector<std::string> &symbolPaths,
		std::string &error)
	{
		std::unique_ptr<SimpleSymbolSupplier> symbolSupplier;
		if (!symbolPaths.empty()) {
			symbolSupplier.reset(new SimpleSymbolSupplier(symbolPaths));
		}

		BasicSourceLineResolver resolver;
		MinidumpProcessor processor(symbolSupplier.get(), &resolver);
		loadedDump.processState = ProcessState();

		{
			StderrInhibitor stderrInhibitor;
			ProcessResult result = processor.Process(&loadedDump.minidump, &loadedDump.processState);
			if (result != google_breakpad::PROCESS_OK) {
				std::ostringstream out;
				out << "Symbolized MinidumpProcessor failed with status " << result;
				error = out.str();
				return false;
			}
		}
		return true;
	}

	const CallStack *GetRequestingThreadStack(const ProcessState &processState, int &threadIndex)
	{
		threadIndex = processState.requesting_thread();
		if (threadIndex < 0) {
			threadIndex = 0;
		}

		const auto *threads = processState.threads();
		if (!threads || threads->empty() || threadIndex >= static_cast<int>(threads->size())) {
			return nullptr;
		}
		return threads->at(threadIndex);
	}

	std::string RenderShortStackTrace(const ProcessState &processState)
	{
		std::ostringstream out;
		out << "Crash reason: " << processState.crash_reason() << "\n";
		out << "Crash address: 0x" << std::hex << processState.crash_address() << std::dec << "\n";

		int threadIndex = 0;
		const CallStack *stack = GetRequestingThreadStack(processState, threadIndex);
		if (!stack) {
			out << "No crashed thread stack is available.\n";
			return out.str();
		}

		out << "Crashed thread: " << threadIndex << "\n";
		const auto *frames = stack->frames();
		for (size_t i = 0; i < frames->size(); ++i) {
			out << "#" << i << " " << RenderFrame(frames->at(i)) << "\n";
		}

		return out.str();
	}

	std::string RenderVerboseStack(const LoadedDump &loadedDump)
	{
		std::ostringstream out;
		const ProcessState &processState = loadedDump.processState;
		const std::string cpu = processState.system_info() ? processState.system_info()->cpu : "";

		int threadIndex = 0;
		const CallStack *stack = GetRequestingThreadStack(processState, threadIndex);
		if (!stack) {
			out << "No crashed thread stack is available.\n";
			return out.str();
		}

		out << processState.crash_reason() << " accessing 0x" << std::hex << processState.crash_address() << std::dec << "\n\n";
		out << "Thread " << threadIndex << " (crashed):\n\n";

		const auto *threadMemoryRegions = processState.thread_memory_regions();
		const MemoryRegion *threadMemory = nullptr;
		if (threadMemoryRegions && threadIndex >= 0 && threadIndex < static_cast<int>(threadMemoryRegions->size())) {
			threadMemory = threadMemoryRegions->at(threadIndex);
		}

		const auto *frames = stack->frames();
		for (size_t i = 0; i < frames->size(); ++i) {
			const StackFrame *frame = frames->at(i);
			std::string prefix = std::to_string(i) + ": ";
			std::string indent = "  " + std::string(prefix.size(), ' ');

			out << "  " << prefix << RenderFrame(frame) << "\n";
			AppendRegisterDump(out, frame, indent);

			if (threadMemory && i + 1 < frames->size()) {
				std::optional<std::vector<uint8_t>> stackBytes = GetFrameStackContents(frame, frames->at(i + 1), cpu, threadMemory);
				if (stackBytes && !stackBytes->empty()) {
					uint64_t stackBase = 0;
					if (cpu == "x86") {
						stackBase = static_cast<const StackFrameX86 *>(frame)->context.esp;
					} else if (cpu == "amd64") {
						stackBase = static_cast<const StackFrameAMD64 *>(frame)->context.rsp;
					}
					out << CollectHexDump(stackBase, *stackBytes, indent);
				}
			}

			out << indent << "Found via " << FrameTrustName(frame->trust) << "\n\n";
		}

		return out.str();
	}

	std::string RenderMemoryDump(Minidump &minidump)
	{
		std::ostringstream out;
		MinidumpMemoryList *memoryList = minidump.GetMemoryList();
		if (!memoryList || memoryList->region_count() == 0) {
			out << "No memory regions in input\n";
			return out.str();
		}

		uint64_t start = (std::numeric_limits<uint64_t>::max)();
		uint64_t end = 0;
		uint64_t totalBytes = 0;
		for (unsigned int i = 0; i < memoryList->region_count(); ++i) {
			MinidumpMemoryRegion *region = memoryList->GetMemoryRegionAtIndex(i);
			start = (std::min)(start, region->GetBase());
			end = (std::max)(end, region->GetBase() + region->GetSize());
			totalBytes += region->GetSize();
		}

		uint64_t covered = end > start ? (end - start) : 0;
		double coverage = covered > 0 ? (static_cast<double>(totalBytes) / static_cast<double>(covered)) * 100.0 : 100.0;
		const size_t addressWidth = HexWidthForAddress(end);
		out << "Got " << totalBytes << " bytes of memory covering "
			<< std::hex << std::setw(static_cast<int>(addressWidth)) << std::setfill('0') << start << " to "
			<< std::setw(static_cast<int>(addressWidth)) << end << std::dec << std::setfill(' ')
			<< " (" << std::fixed << std::setprecision(2) << coverage << "% coverage)\n";

		for (unsigned int i = 0; i < memoryList->region_count(); ++i) {
			MinidumpMemoryRegion *region = memoryList->GetMemoryRegionAtIndex(i);
			const uint8_t *buffer = region->GetMemory();
			if (!buffer || region->GetSize() == 0) {
				continue;
			}

			std::vector<uint8_t> bytes(buffer, buffer + region->GetSize());
			out << "\nRegion " << i << ": base=0x" << std::hex << region->GetBase() << std::dec << " size=" << region->GetSize() << "\n";
			out << CollectHexDump(region->GetBase(), bytes, "  ");
		}

		return out.str();
	}

	std::string RenderRawJson(LoadedDump &loadedDump)
	{
		std::ostringstream out;
		const ProcessState &processState = loadedDump.processState;

		out << "{\n";
		out << "  \"input_file\": \"" << EscapeJson(loadedDump.minidump.path()) << "\",\n";
		out << "  \"crashed\": " << (processState.crashed() ? "true" : "false") << ",\n";
		out << "  \"crash_reason\": \"" << EscapeJson(processState.crash_reason()) << "\",\n";
		out << "  \"crash_address\": " << processState.crash_address() << ",\n";
		out << "  \"requesting_thread\": " << processState.requesting_thread() << ",\n";
		out << "  \"threads\": [\n";

		const auto *threads = processState.threads();
		for (size_t threadIndex = 0; threads && threadIndex < threads->size(); ++threadIndex) {
			const CallStack *stack = threads->at(threadIndex);
			out << "    {\n";
			out << "      \"index\": " << threadIndex << ",\n";
			out << "      \"frames\": [\n";

			const auto *frames = stack->frames();
			for (size_t frameIndex = 0; frameIndex < frames->size(); ++frameIndex) {
				const StackFrame *frame = frames->at(frameIndex);
				out << "        {"
					<< "\"frame\": " << frameIndex
					<< ", \"rendered\": \"" << EscapeJson(RenderFrame(frame)) << "\"";
				if (frame->module) {
					out << ", \"module\": \"" << EscapeJson(PathnameStripper::File(frame->module->code_file())) << "\"";
				}
				if (!frame->function_name.empty()) {
					out << ", \"function\": \"" << EscapeJson(frame->function_name) << "\"";
				}
				out << "}";
				if (frameIndex + 1 < frames->size()) {
					out << ",";
				}
				out << "\n";
			}

			out << "      ]\n";
			out << "    }";
			if (threadIndex + 1 < threads->size()) {
				out << ",";
			}
			out << "\n";
		}

		out << "  ],\n";
		out << "  \"memory_regions\": [\n";

		MinidumpMemoryList *memoryList = loadedDump.minidump.GetMemoryList();
		for (unsigned int i = 0; memoryList && i < memoryList->region_count(); ++i) {
			MinidumpMemoryRegion *region = memoryList->GetMemoryRegionAtIndex(i);
			out << "    {\"base\": " << region->GetBase() << ", \"size\": " << region->GetSize() << "}";
			if (i + 1 < memoryList->region_count()) {
				out << ",";
			}
			out << "\n";
		}

		out << "  ]\n";
		out << "}\n";
		return out.str();
	}

	std::string StripTrailingNewlinesCopy(const std::string &input)
	{
		std::string result = input;
		while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
			result.pop_back();
		}
		return result;
	}

	std::string StripLeadingNonJson(const std::string &raw)
	{
		size_t objectPos = raw.find('{');
		size_t arrayPos = raw.find('[');
		size_t jsonPos = std::string::npos;
		if (objectPos != std::string::npos && arrayPos != std::string::npos) {
			jsonPos = (std::min)(objectPos, arrayPos);
		} else if (objectPos != std::string::npos) {
			jsonPos = objectPos;
		} else {
			jsonPos = arrayPos;
		}

		if (jsonPos == std::string::npos) {
			return raw;
		}

		return raw.substr(jsonPos);
	}

	std::string ExtractConsoleHistorySection(const std::string &metadata)
	{
		static const std::string beginMarker = "-------- CONSOLE HISTORY BEGIN --------\n";
		static const std::string endMarker = "-------- CONSOLE HISTORY END --------";

		size_t begin = metadata.find(beginMarker);
		if (begin == std::string::npos) {
			return "";
		}
		begin += beginMarker.size();

		size_t end = metadata.find(endMarker, begin);
		if (end == std::string::npos) {
			end = metadata.size();
		}

		return StripTrailingNewlinesCopy(metadata.substr(begin, end - begin));
	}

	std::string StripConsoleHistorySection(const std::string &metadata)
	{
		static const std::string beginMarker = "-------- CONSOLE HISTORY BEGIN --------\n";
		size_t begin = metadata.find(beginMarker);
		if (begin == std::string::npos) {
			return metadata;
		}

		return StripTrailingNewlinesCopy(metadata.substr(0, begin));
	}

	std::string BuildTraceReport(const PendingDumpEntry &entry, const LoadedDump &loadedDump, bool includeConsoleHistory);

	std::string BuildTraceReport(const PendingDumpEntry &entry, const LoadedDump &loadedDump)
	{
		return BuildTraceReport(entry, loadedDump, true);
	}

	std::string BuildTraceReport(const PendingDumpEntry &entry, const LoadedDump &loadedDump, bool includeConsoleHistory)
	{
		std::ostringstream out;
		out << RenderShortStackTrace(loadedDump.processState);
		if (entry.hasMetadata) {
			std::string metadata = ReadWholeFile(entry.metadataPath);
			if (!metadata.empty()) {
				std::string filteredMetadata = includeConsoleHistory ? metadata : StripConsoleHistorySection(metadata);
				if (filteredMetadata.empty()) {
					return out.str();
				}
				out << (includeConsoleHistory ? "\n-------- METADATA / CONSOLE --------\n" : "\n-------- METADATA --------\n");
				out << filteredMetadata;
				if (filteredMetadata.back() != '\n') {
					out << "\n";
				}
			}
		}
		return out.str();
	}

	bool IsSupportedDumpMode(const std::string &mode)
	{
		return mode == "trace" || mode == "rawstack" || mode == "rawmemory" || mode == "rawraw" || mode == "all";
	}

	std::string ResolveOutputPath(const PendingDumpEntry &entry,
		const std::string &requestedOutput,
		const std::string &normalizedMode,
		const std::string &outputRoot)
	{
		if (!requestedOutput.empty()) {
			if (IsAbsolutePath(requestedOutput)) {
				return NormalizePathString(std::filesystem::u8path(requestedOutput));
			}
			return NormalizePathString(std::filesystem::u8path(outputRoot) / std::filesystem::u8path(requestedOutput));
		}

		return NormalizePathString(std::filesystem::u8path(outputRoot) / (normalizedMode + "_" + entry.name + ".txt"));
	}

	bool BuildOutputForMode(const PendingDumpEntry &entry,
		LoadedDump &loadedDump,
		const LocalDumpSettings &settings,
		const std::string &normalizedMode,
		std::string &output)
	{
		std::string carburetorRaw;
		std::string carburetorError;
		bool hasCarburetorRaw = false;
		json carburetorDocument;
		bool hasCarburetorJson = false;
		if (normalizedMode == "rawstack" || normalizedMode == "rawmemory" || normalizedMode == "rawraw" || normalizedMode == "all") {
			hasCarburetorRaw = TryRunCarburetorRaw(entry, settings, carburetorRaw, carburetorError);
			if (hasCarburetorRaw) {
				hasCarburetorJson = ParseCarburetorJson(carburetorRaw, carburetorDocument, carburetorError);
			}
		}

		if (normalizedMode == "trace") {
			output = BuildTraceReport(entry, loadedDump);
			return true;
		}
		if (normalizedMode == "rawstack") {
			if (hasCarburetorJson) {
				output = RenderCarburetorStack(carburetorDocument);
			} else {
				output = RenderVerboseStack(loadedDump);
				if (!carburetorError.empty()) {
					output = std::string("Carburetor unavailable, using built-in raw stack view.\nReason: ") + carburetorError + "\n\n" + output;
				}
			}
			return true;
		}
		if (normalizedMode == "rawmemory") {
			if (hasCarburetorJson) {
				std::string memoryError;
				output = RenderCarburetorMemory(carburetorDocument, memoryError);
				if (output.empty()) {
					output = RenderMemoryDump(loadedDump.minidump);
					const std::string reason = !memoryError.empty() ? memoryError : carburetorError;
					if (!reason.empty()) {
						output = std::string("Carburetor unavailable, using built-in raw memory view.\nReason: ") + reason + "\n\n" + output;
					}
				}
			} else {
				output = RenderMemoryDump(loadedDump.minidump);
				if (!carburetorError.empty()) {
					output = std::string("Carburetor unavailable, using built-in raw memory view.\nReason: ") + carburetorError + "\n\n" + output;
				}
			}
			return true;
		}
		if (normalizedMode == "rawraw") {
			output = hasCarburetorRaw ? carburetorRaw : RenderRawJson(loadedDump);
			if (!hasCarburetorRaw && !carburetorError.empty()) {
				output = std::string("Carburetor unavailable, using built-in raw view.\nReason: ") + carburetorError + "\n\n" + output;
			}
			return true;
		}
		if (normalizedMode == "all") {
			std::ostringstream out;
			out << "======== TRACE ========\n" << BuildTraceReport(entry, loadedDump)
				<< "\n======== RAWSTACK ========\n";
			if (hasCarburetorJson) {
				out << RenderCarburetorStack(carburetorDocument);
			} else {
				if (!carburetorError.empty()) {
					out << "Carburetor unavailable, using built-in raw stack view.\nReason: " << carburetorError << "\n\n";
				}
				out << RenderVerboseStack(loadedDump);
			}

			out << "\n======== RAWMEMORY ========\n";
			if (hasCarburetorJson) {
				std::string memoryError;
				std::string memoryOutput = RenderCarburetorMemory(carburetorDocument, memoryError);
				if (!memoryOutput.empty()) {
					out << memoryOutput;
				} else {
					if (!memoryError.empty()) {
						out << "Carburetor unavailable, using built-in raw memory view.\nReason: " << memoryError << "\n\n";
					}
					out << RenderMemoryDump(loadedDump.minidump);
				}
			} else {
				if (!carburetorError.empty()) {
					out << "Carburetor unavailable, using built-in raw memory view.\nReason: " << carburetorError << "\n\n";
				}
				out << RenderMemoryDump(loadedDump.minidump);
			}

			out
				<< "\n======== RAWRAW ========\n";
			if (hasCarburetorRaw) {
				out << carburetorRaw;
			} else {
				if (!carburetorError.empty()) {
					out << "Carburetor unavailable, using built-in raw view.\nReason: " << carburetorError << "\n\n";
				}
				out << RenderRawJson(loadedDump);
			}
			output = out.str();
			return true;
		}
		return false;
	}

	void LogMultilineResult(const char *header, const std::string &text)
	{
		if (header && header[0]) {
			AcceleratorConsoleWarning("%s", header);
		}

		size_t start = 0;
		while (start < text.size()) {
			size_t end = text.find('\n', start);
			std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!line.empty()) {
				AcceleratorConsoleWarning("%s", line.c_str());
			}
			if (end == std::string::npos) {
				break;
			}
			start = end + 1;
		}
	}
}

bool Accelerator_LocalListDumps(std::string &output, std::string &error)
{
	std::vector<PendingDumpEntry> entries;
	if (!CollectPendingDumps(entries, &error)) {
		return false;
	}

	std::ostringstream out;
	if (entries.empty()) {
		out << "No pending dump files were found.\n";
		output = out.str();
		return true;
	}

	out << "Found " << static_cast<unsigned>(entries.size()) << " pending dump(s):\n";
	for (const PendingDumpEntry &entry : entries) {
		out << "  " << entry.name << (entry.hasMetadata ? " [metadata]" : "") << "\n";
	}
	output = out.str();
	return true;
}

namespace
{
	std::mutex localDumpJobMutex;
	std::condition_variable localDumpJobCv;
	std::map<int, std::shared_ptr<LocalDumpJob>> localDumpJobs;
	std::deque<int> localDumpJobQueue;
	std::thread localDumpWorkerThread;
	std::atomic_bool localDumpWorkerStop{false};
	int nextLocalDumpJobId = 1;
	constexpr size_t kLocalDumpFinishedRetention = 32;

	const char *LocalDumpJobStateName(LocalDumpJobState state)
	{
		switch (state) {
			case LocalDumpJobState::Queued: return "queued";
			case LocalDumpJobState::Running: return "running";
			case LocalDumpJobState::Done: return "done";
			case LocalDumpJobState::Failed: return "failed";
		}
		return "unknown";
	}

	LocalDumpSettings CollectCurrentLocalDumpSettings()
	{
		LocalDumpSettings settings;
		settings.gamePath = crashGamePath;
		settings.sourceModPath = crashSourceModPath;
		settings.carburetorPath = GetConfiguredCarburetorPath();
		settings.symbolPaths = GetConfiguredLocalSymbolPaths();
		settings.localSymbolStoreRoot = GetLocalSymbolStoreRoot();
		settings.localOutputRoot = GetLocalOutputRoot();
#if defined _WINDOWS
		settings.dumpSymsPath = NormalizePathString(std::filesystem::u8path(crashSourceModPath) / "bin" / "dump_syms.exe");
#endif
		return settings;
	}

	void TrimFinishedLocalDumpJobsLocked()
	{
		while (localDumpJobs.size() > kLocalDumpFinishedRetention) {
			auto it = std::find_if(localDumpJobs.begin(), localDumpJobs.end(), [](const auto &entry) {
				return entry.second->state == LocalDumpJobState::Done || entry.second->state == LocalDumpJobState::Failed;
			});
			if (it == localDumpJobs.end()) {
				break;
			}
			localDumpJobs.erase(it);
		}
	}

	bool BuildLocalDumpTask(LocalDumpJob &job, std::string &error)
	{
		if (job.dumpName.empty()) {
			error = "Dump name is empty";
			return false;
		}
		if (!job.stackOnly && !IsSupportedDumpMode(job.mode)) {
			error = "Invalid dump mode";
			return false;
		}
		if (!FindPendingDumpByName(job.dumpName, job.entry, &error)) {
			return false;
		}

		job.settings = CollectCurrentLocalDumpSettings();
		if (!job.stackOnly) {
			job.outputPath = ResolveOutputPath(job.entry, job.requestedOutputPath, job.mode, job.settings.localOutputRoot);
		}
		return true;
	}

	bool ExecuteLocalDumpTask(LocalDumpJob &job, std::string &error)
	{
		LoadedDump loadedDump(job.entry.dumpPath);
		if (!LoadDump(loadedDump, error)) {
			return false;
		}

		EnsureLocalSymbolsForDump(loadedDump, job.settings);
		if (!ReloadDumpWithLocalSymbols(loadedDump, job.settings.symbolPaths, error)) {
			return false;
		}

		if (job.stackOnly) {
			job.result = BuildTraceReport(job.entry, loadedDump, false);
			std::ostringstream out;
			out << "Built stack dump for " << job.dumpName;
			job.status = out.str();
			return true;
		}

		std::string output;
		if (!BuildOutputForMode(job.entry, loadedDump, job.settings, job.mode, output)) {
			error = "Unsupported mode";
			return false;
		}

		std::string directoryError;
		if (!EnsureDirectoryExists(std::filesystem::path(job.outputPath).parent_path().string(), directoryError)) {
			error = directoryError;
			return false;
		}
		if (!WriteWholeFile(job.outputPath, output, &error)) {
			return false;
		}

		std::ostringstream out;
		out << "Wrote " << job.mode << " output for " << job.dumpName << " to " << job.outputPath;
		job.status = out.str();
		job.result = job.status;
		return true;
	}

	void LocalDumpWorkerLoop()
	{
		for (;;) {
			std::shared_ptr<LocalDumpJob> job;
			{
				std::unique_lock<std::mutex> lock(localDumpJobMutex);
				localDumpJobCv.wait(lock, []() {
					return localDumpWorkerStop.load() || !localDumpJobQueue.empty();
				});

				if (localDumpWorkerStop.load() && localDumpJobQueue.empty()) {
					return;
				}

				int jobId = localDumpJobQueue.front();
				localDumpJobQueue.pop_front();

				auto it = localDumpJobs.find(jobId);
				if (it == localDumpJobs.end()) {
					continue;
				}

				job = it->second;
				job->state = LocalDumpJobState::Running;
				job->status = "Job is running";
			}

			AcceleratorConsoleWarning("Local dump job #%d started: %s%s",
				job->id,
				job->dumpName.c_str(),
				job->stackOnly ? " [stack]" : "");

			std::string error;
			const bool succeeded = ExecuteLocalDumpTask(*job, error);

			{
				std::lock_guard<std::mutex> lock(localDumpJobMutex);
				job->finishedAt = std::chrono::system_clock::now();
				if (succeeded) {
					job->state = LocalDumpJobState::Done;
				} else {
					job->state = LocalDumpJobState::Failed;
					job->error = error;
					job->status = error;
				}
				TrimFinishedLocalDumpJobsLocked();
			}

			if (succeeded) {
				AcceleratorConsoleWarning("Local dump job #%d finished: %s", job->id, job->status.c_str());
				if (job->stackOnly) {
					std::ostringstream header;
					header << "Local dump job #" << job->id << " stack trace for " << job->dumpName << ":";
					LogMultilineResult(header.str().c_str(), job->result);
				} else {
					AcceleratorConsoleWarning("Local dump job #%d result: %s", job->id, job->result.c_str());
				}
			} else {
				AcceleratorConsoleWarning("Local dump job #%d failed: %s", job->id, error.c_str());
			}
		}
	}

	void StartLocalDumpWorker()
	{
		localDumpWorkerStop.store(false);
		if (!localDumpWorkerThread.joinable()) {
			localDumpWorkerThread = std::thread(LocalDumpWorkerLoop);
		}
	}

	void StopLocalDumpWorker()
	{
		localDumpWorkerStop.store(true);
		localDumpJobCv.notify_all();
		if (localDumpWorkerThread.joinable()) {
			localDumpWorkerThread.join();
		}
	}

	bool EnqueueLocalDumpJob(std::shared_ptr<LocalDumpJob> job, int &jobId, std::string &status, std::string &error)
	{
		if (!job) {
			error = "Job allocation failed";
			return false;
		}
		if (!BuildLocalDumpTask(*job, error)) {
			return false;
		}

		{
			std::lock_guard<std::mutex> lock(localDumpJobMutex);
			job->id = nextLocalDumpJobId++;
			job->createdAt = std::chrono::system_clock::now();
			job->state = LocalDumpJobState::Queued;
			job->status = "Job is queued";
			localDumpJobs[job->id] = job;
			localDumpJobQueue.push_back(job->id);
			AcceleratorConsoleWarning("Local dump job #%d queued: %s%s",
				job->id,
				job->dumpName.c_str(),
				job->stackOnly ? " [stack]" : "");
		}

		localDumpJobCv.notify_one();

		jobId = job->id;
		std::ostringstream out;
		out << "Started local dump job #" << jobId << " for " << job->dumpName;
		status = out.str();
		return true;
	}
}

bool Accelerator_LocalListJobs(std::string &output, std::string &error)
{
	std::lock_guard<std::mutex> lock(localDumpJobMutex);
	std::ostringstream out;
	if (localDumpJobs.empty()) {
		out << "No local dump jobs exist.\n";
		output = out.str();
		return true;
	}

	out << "Found " << static_cast<unsigned>(localDumpJobs.size()) << " local dump job(s):\n";
	for (const auto &entry : localDumpJobs) {
		const LocalDumpJob &job = *entry.second;
		out << "  #" << job.id << " [" << LocalDumpJobStateName(job.state) << "] "
			<< job.dumpName;
		if (!job.mode.empty()) {
			out << " mode=" << job.mode;
		} else if (job.stackOnly) {
			out << " mode=stack";
		}
		if (!job.outputPath.empty()) {
			out << " output=" << job.outputPath;
		}
		out << "\n";
	}
	output = out.str();
	return true;
}

bool Accelerator_LocalProcessDump(const char *dumpName,
	const char *mode,
	const char *outputName,
	std::string &outputPath,
	std::string &status,
	std::string &error)
{
	std::string dumpNameString = dumpName ? dumpName : "";
	std::string modeString = ToLowerCopy(mode ? mode : "");
	std::string outputNameString = outputName ? outputName : "";

	if (dumpNameString.empty() || !IsSupportedDumpMode(modeString)) {
		error = "Invalid dump name or mode";
		return false;
	}

	PendingDumpEntry entry;
	if (!FindPendingDumpByName(dumpNameString, entry, &error)) {
		return false;
	}

	LoadedDump loadedDump(entry.dumpPath);
	if (!LoadDump(loadedDump, error)) {
		return false;
	}

	const LocalDumpSettings settings = CollectCurrentLocalDumpSettings();
	EnsureLocalSymbolsForDump(loadedDump, settings);
	if (!ReloadDumpWithLocalSymbols(loadedDump, settings.symbolPaths, error)) {
		return false;
	}

	std::string output;
	if (!BuildOutputForMode(entry, loadedDump, settings, modeString, output)) {
		error = "Unsupported mode";
		return false;
	}

	outputPath = ResolveOutputPath(entry, outputNameString, modeString, settings.localOutputRoot);
	std::string directoryError;
	if (!EnsureDirectoryExists(std::filesystem::path(outputPath).parent_path().string(), directoryError)) {
		error = directoryError;
		return false;
	}
	if (!WriteWholeFile(outputPath, output, &error)) {
		return false;
	}

	std::ostringstream out;
	out << "Wrote " << modeString << " output for " << dumpNameString << " to " << outputPath;
	status = out.str();
	return true;
}

bool Accelerator_LocalStartProcessDump(const char *dumpName,
	const char *mode,
	const char *outputName,
	int &jobId,
	std::string &status,
	std::string &error)
{
	auto job = std::make_shared<LocalDumpJob>();
	job->dumpName = dumpName ? dumpName : "";
	job->mode = ToLowerCopy(mode ? mode : "");
	job->requestedOutputPath = outputName ? outputName : "";
	job->stackOnly = false;
	return EnqueueLocalDumpJob(job, jobId, status, error);
}

bool Accelerator_LocalGetStackDump(const char *dumpName, std::string &stackTrace, std::string &error)
{
	std::string dumpNameString = dumpName ? dumpName : "";
	if (dumpNameString.empty()) {
		error = "Dump name is empty";
		return false;
	}

	PendingDumpEntry entry;
	if (!FindPendingDumpByName(dumpNameString, entry, &error)) {
		return false;
	}

	LoadedDump loadedDump(entry.dumpPath);
	if (!LoadDump(loadedDump, error)) {
		return false;
	}

	const LocalDumpSettings settings = CollectCurrentLocalDumpSettings();
	EnsureLocalSymbolsForDump(loadedDump, settings);
	if (!ReloadDumpWithLocalSymbols(loadedDump, settings.symbolPaths, error)) {
		return false;
	}

	stackTrace = BuildTraceReport(entry, loadedDump, false);
	return true;
}

bool Accelerator_LocalStartStackDump(const char *dumpName, int &jobId, std::string &status, std::string &error)
{
	auto job = std::make_shared<LocalDumpJob>();
	job->dumpName = dumpName ? dumpName : "";
	job->stackOnly = true;
	return EnqueueLocalDumpJob(job, jobId, status, error);
}

bool Accelerator_LocalGetJobStatus(int jobId,
	std::string &state,
	std::string &status,
	std::string &outputPath,
	std::string &error)
{
	std::lock_guard<std::mutex> lock(localDumpJobMutex);
	auto it = localDumpJobs.find(jobId);
	if (it == localDumpJobs.end()) {
		error = "Job not found";
		return false;
	}

	const LocalDumpJob &job = *it->second;
	state = LocalDumpJobStateName(job.state);
	status = job.state == LocalDumpJobState::Failed ? job.error : job.status;
	outputPath = job.outputPath;
	return true;
}

bool Accelerator_LocalGetJobResult(int jobId, std::string &result, std::string &error)
{
	std::lock_guard<std::mutex> lock(localDumpJobMutex);
	auto it = localDumpJobs.find(jobId);
	if (it == localDumpJobs.end()) {
		error = "Job not found";
		return false;
	}

	const LocalDumpJob &job = *it->second;
	if (job.state == LocalDumpJobState::Queued || job.state == LocalDumpJobState::Running) {
		error = "Job is not finished yet";
		return false;
	}
	if (job.state == LocalDumpJobState::Failed) {
		error = job.error.empty() ? "Job failed" : job.error;
		return false;
	}
	if (job.stackOnly) {
		result = job.result;
	} else {
		std::ostringstream out;
		out << job.status;
		result = out.str();
	}
	return true;
}

bool Accelerator_LocalGetConsoleDump(const char *dumpName, std::string &consoleDump, std::string &error)
{
	std::string dumpNameString = dumpName ? dumpName : "";
	if (dumpNameString.empty()) {
		error = "Dump name is empty";
		return false;
	}

	PendingDumpEntry entry;
	if (!FindPendingDumpByName(dumpNameString, entry, &error)) {
		return false;
	}
	if (!entry.hasMetadata) {
		error = "Dump metadata file was not found";
		return false;
	}

	std::string metadata = ReadWholeFile(entry.metadataPath);
	if (metadata.empty()) {
		error = "Dump metadata file is empty";
		return false;
	}

	consoleDump = ExtractConsoleHistorySection(metadata);
	if (consoleDump.empty()) {
		error = "Console history was not found in dump metadata";
		return false;
	}
	return true;
}

void Accelerator_LocalTriggerCrashTest()
{
	AcceleratorConsoleWarning("Crash test command invoked. The server will now crash intentionally for Accelerator verification.");
	fflush(stderr);

#if defined _WINDOWS
	RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#else
	volatile int *crash = nullptr;
	*crash = 0xA11;
#endif
}

struct UploadWatchdog
{
	std::atomic_bool done{false};
	std::string stage;
	std::string url;
};

static std::shared_ptr<UploadWatchdog> StartUploadWatchdog(const char *stage, const char *url)
{
	auto watchdog = std::make_shared<UploadWatchdog>();
	watchdog->stage = stage ? stage : "upload";
	watchdog->url = RedactUrlForLog(url);

	std::thread([watchdog]() {
		std::this_thread::sleep_for(std::chrono::seconds(5));
		if (!watchdog->done.load()) {
			AcceleratorConsoleWarning("%s did not finish within 5 seconds: %s", watchdog->stage.c_str(), watchdog->url.c_str());
		}
	}).detach();

	return watchdog;
}

static void FinishUploadWatchdog(const std::shared_ptr<UploadWatchdog>& watchdog)
{
	if (watchdog) {
		watchdog->done.store(true);
	}
}

#if defined _LINUX
void terminateHandler()
{
	const char *msg = "missing exception";
	std::exception_ptr pEx = std::current_exception();
	if (pEx) {
		try {
			std::rethrow_exception(pEx);
		} catch(const std::exception &e) {
			msg = strdup(e.what());
		} catch(...) {
			msg = "unknown exception";
		}
	}

	size_t msgLength = strlen(msg) + 2;
	volatile char * volatile msgForCrashDumps = (char *)alloca(msgLength);
	strcpy((char *)msgForCrashDumps + 1, msg);

	abort();
}

void (*SignalHandler)(int, siginfo_t *, void *);

const int kExceptionSignals[] = {
	SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS
};

const int kNumHandledSignals = sizeof(kExceptionSignals) / sizeof(kExceptionSignals[0]);

static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	//printf("Wrote minidump to: %s\n", descriptor.path());

	if (succeeded) {
		sys_write(STDOUT_FILENO, "Wrote minidump to: ", 19);
	} else {
		sys_write(STDOUT_FILENO, "Failed to write minidump to: ", 29);
	}

	sys_write(STDOUT_FILENO, descriptor.path(), my_strlen(descriptor.path()));
	sys_write(STDOUT_FILENO, "\n", 1);

	if (!succeeded) {
		return succeeded;
	}

	my_strlcpy(dumpStoragePath, descriptor.path(), sizeof(dumpStoragePath));
	my_strlcat(dumpStoragePath, ".txt", sizeof(dumpStoragePath));

	int extra = sys_open(dumpStoragePath, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (extra == -1) {
		sys_write(STDOUT_FILENO, "Failed to open metadata file!\n", 30);
		return succeeded;
	}

	sys_write(extra, "-------- CONFIG BEGIN --------", 30);
	sys_write(extra, "\nMap=", 5);
	sys_write(extra, crashMap, my_strlen(crashMap));
	sys_write(extra, "\nGamePath=", 10);
	sys_write(extra, crashGamePath, my_strlen(crashGamePath));
	sys_write(extra, "\nCommandLine=", 13);
	sys_write(extra, crashCommandLine, my_strlen(crashCommandLine));
	sys_write(extra, "\nSourceModPath=", 15);
	sys_write(extra, crashSourceModPath, my_strlen(crashSourceModPath));
	sys_write(extra, "\nGameDirectory=", 15);
	sys_write(extra, crashGameDirectory, my_strlen(crashGameDirectory));
	if (crashSourceModVersion[0]) {
		sys_write(extra, "\nSourceModVersion=", 18);
		sys_write(extra, crashSourceModVersion, my_strlen(crashSourceModVersion));
	}
	sys_write(extra, "\nExtensionVersion=", 18);
	sys_write(extra, SM_VERSION, my_strlen(SM_VERSION));
	sys_write(extra, "\nExtensionBuild=", 16);
	sys_write(extra, SM_BUILD_UNIQUEID, my_strlen(SM_BUILD_UNIQUEID));
	sys_write(extra, steamInf, my_strlen(steamInf));
	sys_write(extra, "\n-------- CONFIG END --------\n", 30);

	if (GetSpew) {
		GetSpew(spewBuffer, sizeof(spewBuffer));

		if (my_strlen(spewBuffer) > 0) {
			sys_write(extra, "-------- CONSOLE HISTORY BEGIN --------\n", 40);
			sys_write(extra, spewBuffer, my_strlen(spewBuffer));
			sys_write(extra, "-------- CONSOLE HISTORY END --------\n", 38);
		}
	}

	sys_close(extra);

	return succeeded;
}

void OnGameFrame(bool simulating)
{
	std::set_terminate(terminateHandler);

	bool weHaveBeenFuckedOver = false;
	struct sigaction oact;

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaction(kExceptionSignals[i], NULL, &oact);

		if (oact.sa_sigaction != SignalHandler) {
			weHaveBeenFuckedOver = true;
			break;
		}
	}

	if (!weHaveBeenFuckedOver) {
		return;
	}

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaddset(&act.sa_mask, kExceptionSignals[i]);
	}

	act.sa_sigaction = SignalHandler;
	act.sa_flags = SA_ONSTACK | SA_SIGINFO;

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaction(kExceptionSignals[i], &act, NULL);
	}
}

#elif defined _WINDOWS
void *vectoredHandler = NULL;

LONG CALLBACK BreakpadVectoredHandler(_In_ PEXCEPTION_POINTERS ExceptionInfo)
{
	switch (ExceptionInfo->ExceptionRecord->ExceptionCode)
	{
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_INVALID_HANDLE:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		case EXCEPTION_DATATYPE_MISALIGNMENT:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_STACK_OVERFLOW:
		case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
		case 0xC0000374: // STATUS_HEAP_CORRUPTION
			break;
		case 0: // Valve use this for Sys_Error.
			if ((ExceptionInfo->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) == 0)
				return EXCEPTION_CONTINUE_SEARCH;
			break;
		default:
			return EXCEPTION_CONTINUE_SEARCH;
	}

	if (handler->WriteMinidumpForException(ExceptionInfo))
	{
		// Stop the handler thread from deadlocking us.
		delete handler;

		// Stop Valve's handler being called.
		ExceptionInfo->ExceptionRecord->ExceptionCode = EXCEPTION_BREAKPOINT;

		return EXCEPTION_EXECUTE_HANDLER;
	} else {
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

static bool dumpCallback(const wchar_t* dump_path,
                         const wchar_t* minidump_id,
                         void* context,
                         EXCEPTION_POINTERS* exinfo,
                         MDRawAssertionInfo* assertion,
                         bool succeeded)
{
	if (!succeeded) {
		printf("Failed to write minidump to: %ls\\%ls.dmp\n", dump_path, minidump_id);
		return succeeded;
	}

	printf("Wrote minidump to: %ls\\%ls.dmp\n", dump_path, minidump_id);

	sprintf(dumpStoragePath, "%ls\\%ls.dmp.txt", dump_path, minidump_id);

	FILE *extra = fopen(dumpStoragePath, "wb");
	if (!extra) {
		printf("Failed to open metadata file!\n");
		return succeeded;
	}

	fprintf(extra, "-------- CONFIG BEGIN --------");
	fprintf(extra, "\nMap=%s", crashMap);
	fprintf(extra, "\nGamePath=%s", crashGamePath);
	fprintf(extra, "\nCommandLine=%s", crashCommandLine);
	fprintf(extra, "\nSourceModPath=%s", crashSourceModPath);
	fprintf(extra, "\nGameDirectory=%s", crashGameDirectory);
	if (crashSourceModVersion[0]) {
		fprintf(extra, "\nSourceModVersion=%s", crashSourceModVersion);
	}
	fprintf(extra, "\nExtensionVersion=%s", SM_VERSION);
	fprintf(extra, "\nExtensionBuild=%s", SM_BUILD_UNIQUEID);
	fprintf(extra, "%s", steamInf);
	fprintf(extra, "\n-------- CONFIG END --------\n");

	if (GetSpew || GetSpewFastcall) {
		if (GetSpew) {
			GetSpew(spewBuffer, sizeof(spewBuffer));
		} else if (GetSpewFastcall) {
			GetSpewFastcall(spewBuffer, sizeof(spewBuffer));
		}

		if (spewBuffer[0]) {
			fprintf(extra, "-------- CONSOLE HISTORY BEGIN --------\n%s-------- CONSOLE HISTORY END --------\n", spewBuffer);
		}
	}

	fclose(extra);

	return succeeded;
}

#else
#error Bad platform.
#endif

class ClogInhibitor
{
	std::streambuf *saved_clog = nullptr;

public:
	ClogInhibitor() {
		saved_clog = std::clog.rdbuf();
		std::clog.rdbuf(nullptr);
	}

	~ClogInhibitor() {
		std::clog.rdbuf(saved_clog);
	}
};

class UploadThread: public IThread
{
	char serverId[38] = "";

	void RunThread(IThreadHandle *pHandle) {
		const bool localMode = IsLocalMode();
		AcceleratorDebugLog("Upload thread started. dump path: %s", dumpStoragePath);
		AcceleratorDebugLog("Mode: %s", localMode ? "local" : "site");

		char path[512];
		g_pSM->Format(path, sizeof(path), "%s/server-id.txt", dumpStoragePath);
		FILE *serverIdFile = fopen(path, "r");
		if (serverIdFile) {
			fread(serverId, 1, sizeof(serverId) - 1, serverIdFile);
			if (!feof(serverIdFile) || strlen(serverId) != 36) {
				serverId[0] = '\0';
			}
			fclose(serverIdFile);
		}
		if (!serverId[0]) {
			serverIdFile = fopen(path, "w");
			if (serverIdFile) {
				g_pSM->Format(serverId, sizeof(serverId), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
					rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, 0x40 | ((rand() % 255) & 0x0F), rand() % 255,
					0x80 | ((rand() % 255) & 0x3F), rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255);
				fputs(serverId, serverIdFile);
				fclose(serverIdFile);
			}
		}

		IDirectory *dumps = libsys->OpenDirectory(dumpStoragePath);
		if (!dumps) {
			AcceleratorConsoleWarning("Failed to open dump directory: %s", dumpStoragePath);
			g_accelerator.MarkAsDoneUploading();
			return;
		}

		int skip = 0;
		int count = 0;
		int failed = 0;
		int pending = 0;
		char metapath[512];
		char presubmitToken[512];
		char response[512];

		while (dumps->MoreFiles()) {
			if (!dumps->IsEntryFile()) {
				dumps->NextEntry();
				continue;
			}

			const char *name = dumps->GetEntryName();

			int namelen = strlen(name);
			if (namelen < 4 || strcmp(&name[namelen-4], ".dmp") != 0) {
				dumps->NextEntry();
				continue;
			}

			g_pSM->Format(path, sizeof(path), "%s/%s", dumpStoragePath, name);
			g_pSM->Format(metapath, sizeof(metapath), "%s.txt", path);
			pending++;
			AcceleratorDebugLog("Found pending crash dump: %s", path);

			if (!libsys->PathExists(metapath)) {
				metapath[0] = '\0';
				AcceleratorConsoleWarning("Crash dump metadata file is missing for %s; proceeding without metadata", path);
				AcceleratorDebugLog("Metadata file missing");
			}

			if (localMode) {
				skip++;
				AcceleratorDebugLog("Local mode: skipped remote upload for %s", path);
				dumps->NextEntry();
				continue;
			}

			presubmitToken[0] = '\0';
			PresubmitResponse presubmitResponse = kPRUploadCrashDumpAndMetadata;
			bool deleteAfterProcessing = false;
			const bool shouldDeleteProcessedDump = ShouldDeleteProcessedDump();

			const char *presubmitOption = g_pSM->GetCoreConfigValue("MinidumpPresubmit");
			bool canPresubmit = !presubmitOption || (tolower(presubmitOption[0]) == 'y' || presubmitOption[0] == '1');

			if (canPresubmit) {
				presubmitResponse = PresubmitCrashDump(path, presubmitToken, sizeof(presubmitToken));
			}

			switch (presubmitResponse) {
				case kPRLocalError:
					failed++;
					AcceleratorConsoleWarning("Failed to locally process crash dump before upload.");
					break;
				case kPRRemoteError:
				case kPRUploadCrashDumpAndMetadata:
				case kPRUploadMetadataOnly:
					if (UploadCrashDump((presubmitResponse == kPRUploadMetadataOnly) ? nullptr : path, metapath, presubmitToken, response, sizeof(response))) {
						count++;
						deleteAfterProcessing = shouldDeleteProcessedDump;
						AcceleratorConsoleWarning("Uploaded crash dump: %s", response);
						UploadedCrash crash{ response };
						g_accelerator.StoreUploadedCrash(crash);
					} else {
						failed++;
						AcceleratorConsoleWarning("Failed to upload crash dump: %s", response);
					}
					break;
				case kPRDontUpload:
					skip++;
					deleteAfterProcessing = shouldDeleteProcessedDump;
					AcceleratorConsoleWarning("Crash dump upload skipped by server policy.");
					break;
			}

			if (deleteAfterProcessing) {
				if (metapath[0]) {
					unlink(metapath);
				}

				unlink(path);
			} else {
				const char *keepReason = shouldDeleteProcessedDump
					? "retry"
					: "MinidumpDeleteAfterProcessing is disabled";
				AcceleratorConsoleWarning("Keeping crash dump (%s): %s", keepReason, path);
			}

			dumps->NextEntry();
		}

		libsys->CloseDirectory(dumps);
		if (pending == 0) {
			AcceleratorDebugLog("No pending crash dumps found.");
		}

		g_accelerator.MarkAsDoneUploading();
		if (skip == 0 && count == 0 && failed == 0) {
			AcceleratorConsoleMessage("Upload thread finished. (%d skipped, %d uploaded, %d failed)", skip, count, failed);
		} else {
			AcceleratorConsoleWarning("Upload thread finished. (%d skipped, %d uploaded, %d failed)", skip, count, failed);
		}
	}

	void OnTerminate(IThreadHandle *pHandle, bool cancel) {
		AcceleratorDebugLog("Upload thread terminated. (canceled = %s)", (cancel ? "true" : "false"));
	}

#if defined _LINUX
	bool UploadSymbolFile(const google_breakpad::CodeModule *module, const char *presubmitToken) {
		AcceleratorDebugLog("UploadSymbolFile");

		auto debugFile = module->debug_file();
		std::string vdsoOutputPath = "";

		if (false && debugFile == "linux-gate.so") {
			FILE *auxvFile = fopen("/proc/self/auxv", "rb");
			if (auxvFile) {
				char vdsoOutputPathBuffer[512];
				g_pSM->BuildPath(Path_SM, vdsoOutputPathBuffer, sizeof(vdsoOutputPathBuffer), "data/dumps/linux-gate.so");
				vdsoOutputPath = vdsoOutputPathBuffer;

				while (!feof(auxvFile)) {
					int auxvEntryId = 0;
					fread(&auxvEntryId, sizeof(auxvEntryId), 1, auxvFile);
					long auxvEntryValue = 0;
					fread(&auxvEntryValue, sizeof(auxvEntryValue), 1, auxvFile);

					if (auxvEntryId == 0) break;
					if (auxvEntryId != 33) continue; // AT_SYSINFO_EHDR

#ifdef PLATFORM_X64
					Elf64_Ehdr *vdsoHdr = (Elf64_Ehdr *)auxvEntryValue;
#else
					Elf32_Ehdr *vdsoHdr = (Elf32_Ehdr *)auxvEntryValue;
#endif
					auto vdsoSize = vdsoHdr->e_shoff + (vdsoHdr->e_shentsize * vdsoHdr->e_shnum);
					void *vdsoBuffer = malloc(vdsoSize);
					memcpy(vdsoBuffer, vdsoHdr, vdsoSize);

					FILE *vdsoFile = fopen(vdsoOutputPath.c_str(), "wb");
					if (vdsoFile) {
						fwrite(vdsoBuffer, 1, vdsoSize, vdsoFile);
						fclose(vdsoFile);
						debugFile = vdsoOutputPath;
					}

					free(vdsoBuffer);
					break;
				}

				fclose(auxvFile);
			}
		}

		if (!IsAbsolutePath(debugFile)) {
			return false;
		}

		AcceleratorDebugLog("Submitting symbols for %s", debugFile.c_str());

		auto debugFileDir = google_breakpad::DirName(debugFile);
		std::vector<std::string> debug_dirs{
			debugFileDir,
			debugFileDir + "/.debug",
			"/usr/lib/debug" + debugFileDir,
		};

		std::ostringstream outputStream;
		google_breakpad::DumpOptions options(ALL_SYMBOL_DATA, true, true, false);

		{
			StderrInhibitor stdrrInhibitor;

			if (!WriteSymbolFile(debugFile, debugFile, "Linux", "", debug_dirs, options, outputStream)) {
				outputStream.str("");
				outputStream.clear();

				// Try again without debug dirs.
				if (!WriteSymbolFile(debugFile, debugFile, "Linux", "", {}, options, outputStream)) {
					AcceleratorDebugLog("Failed to process symbol file");
					return false;
				}
			}
		}

		auto output = outputStream.str();
		// output = output.substr(0, output.find("\n"));
		// printf(">>> %s\n", output.c_str());

		if (debugFile == vdsoOutputPath) {
			unlink(vdsoOutputPath.c_str());
		}

		IWebForm *form = webternet->CreateForm();

		const char *minidumpAccount = g_pSM->GetCoreConfigValue("MinidumpAccount");
		if (minidumpAccount && minidumpAccount[0]) form->AddString("UserID", minidumpAccount);

		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		form->AddString("symbol_file", output.c_str());

		MemoryDownloader data;
		IWebTransfer *xfer = webternet->CreateSession();
		xfer->SetFailOnHTTPError(true);

		const char *symbolUrl = g_pSM->GetCoreConfigValue("MinidumpSymbolUrl");
		if (!symbolUrl) symbolUrl = "http://crash.limetech.org/symbols/submit";

		auto watchdog = StartUploadWatchdog("symbol upload", symbolUrl);
		bool symbolUploaded = xfer->PostAndDownload(symbolUrl, form, &data, NULL);
		FinishUploadWatchdog(watchdog);

		if (!symbolUploaded) {
			AcceleratorConsoleWarning("Symbol upload failed for %s via %s: %s (%d)",
				debugFile.c_str(), RedactUrlForLog(symbolUrl).c_str(), xfer->LastErrorMessage(), xfer->LastErrorCode());
			AcceleratorDebugLog("Symbol upload failed: %s (%d)", xfer->LastErrorMessage(), xfer->LastErrorCode());
			return false;
		}

		int responseSize = data.GetSize();
		char *response = new char[responseSize + 1];
		strncpy(response, data.GetBuffer(), responseSize + 1);
		response[responseSize] = '\0';
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		AcceleratorDebugLog("Symbol upload complete: %s", response);
		delete[] response;
		return true;
	}
#endif

	bool UploadModuleFile(const google_breakpad::CodeModule *module, const char *presubmitToken) {
		const auto &codeFile = module->code_file();

		if (!IsAbsolutePath(codeFile)) {
			return false;
		}

		AcceleratorDebugLog("Submitting binary for %s", codeFile.c_str());

		IWebForm *form = webternet->CreateForm();

		const char *minidumpAccount = g_pSM->GetCoreConfigValue("MinidumpAccount");
		if (minidumpAccount && minidumpAccount[0]) form->AddString("UserID", minidumpAccount);

		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		form->AddString("debug_identifier", module->debug_identifier().c_str());
		form->AddString("code_identifier", module->code_identifier().c_str());

		form->AddFile("code_file", codeFile.c_str());

		MemoryDownloader data;
		IWebTransfer *xfer = webternet->CreateSession();
		xfer->SetFailOnHTTPError(true);

		const char *binaryUrl = g_pSM->GetCoreConfigValue("MinidumpBinaryUrl");
		if (!binaryUrl) binaryUrl = "http://crash.limetech.org/binary/submit";

		auto watchdog = StartUploadWatchdog("binary upload", binaryUrl);
		bool binaryUploaded = xfer->PostAndDownload(binaryUrl, form, &data, NULL);
		FinishUploadWatchdog(watchdog);

		if (!binaryUploaded) {
			AcceleratorConsoleWarning("Binary upload failed for %s via %s: %s (%d)",
				codeFile.c_str(), RedactUrlForLog(binaryUrl).c_str(), xfer->LastErrorMessage(), xfer->LastErrorCode());
			AcceleratorDebugLog("Binary upload failed: %s (%d)", xfer->LastErrorMessage(), xfer->LastErrorCode());
			return false;
		}

		int responseSize = data.GetSize();
		char *response = new char[responseSize + 1];
		strncpy(response, data.GetBuffer(), responseSize + 1);
		response[responseSize] = '\0';
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		AcceleratorDebugLog("Binary upload complete: %s", response);
		delete[] response;

		return true;
	}

	enum ModuleType {
		kMTUnknown,
		kMTSystem,
		kMTGame,
		kMTAddon,
		kMTExtension,
	};

	const char *ModuleTypeCode[5] = {
		"Unknown",
		"System",
		"Game",
		"Addon",
		"Extension",
	};

#ifndef WIN32
#define PATH_SEP "/"
#else
#define PATH_SEP "\\"
#endif

	bool PathPrefixMatches(const std::string &prefix, const std::string &path) {
#ifndef WIN32
		return strncmp(prefix.c_str(), path.c_str(), prefix.length()) == 0;
#else
		return _strnicmp(prefix.c_str(), path.c_str(), prefix.length()) == 0;
#endif
	}

	struct PathComparator {
		struct compare {
			bool operator() (const unsigned char &a, const unsigned char &b) const {
#ifndef WIN32
				return a < b;
#else
				return tolower(a) < tolower(b);
#endif
			}
		};

		bool operator() (const std::string &a, const std::string &b) const {
			return !std::lexicographical_compare(
				a.begin(), a.end(),
				b.begin(), b.end(),
				compare());
		};
	};

	std::map<std::string, ModuleType, PathComparator> modulePathMap;
	bool InitModuleClassificationMap(const std::string &base) {
		if (!modulePathMap.empty()) {
			modulePathMap.clear();
		}

		modulePathMap[base] = kMTGame;
		modulePathMap[std::string(crashGamePath) + PATH_SEP "addons" PATH_SEP] = kMTAddon;
		modulePathMap[std::string(crashSourceModPath) + PATH_SEP "extensions" PATH_SEP] = kMTExtension;

		return true;
	}

	ModuleType ClassifyModule(const google_breakpad::CodeModule *module) {
		if (modulePathMap.empty()) {
			return kMTUnknown;
		}

		const auto &codeFile = module->code_file();

		if (codeFile == "linux-gate.so") {
			return kMTSystem;
		}

		if (!IsAbsolutePath(codeFile)) {
			return kMTUnknown;
		}

		for (decltype(modulePathMap)::const_iterator i = modulePathMap.begin(); i != modulePathMap.end(); ++i) {
			if (PathPrefixMatches(i->first, codeFile)) {
				return i->second;
			}
		}

		return kMTSystem;
	}

	std::string PathnameStripper_Directory(const std::string &path) {
		std::string::size_type slash = path.rfind('/');
		std::string::size_type backslash = path.rfind('\\');

		std::string::size_type file_start = 0;
		if (slash != std::string::npos && (backslash == std::string::npos || slash > backslash)) {
			file_start = slash + 1;
		} else if (backslash != std::string::npos) {
			file_start = backslash + 1;
		}

		return path.substr(0, file_start);
	}

	enum PresubmitResponse {
		kPRLocalError,
		kPRRemoteError,
		kPRDontUpload,
		kPRUploadCrashDumpAndMetadata,
		kPRUploadMetadataOnly,
	};

	PresubmitResponse PresubmitCrashDump(const char *path, char *tokenBuffer, size_t tokenBufferLength) {
		google_breakpad::ProcessState processState;
		google_breakpad::ProcessResult processResult;
		google_breakpad::MinidumpProcessor minidumpProcessor(nullptr, nullptr);

		{
			ClogInhibitor clogInhibitor;
			processResult = minidumpProcessor.Process(path, &processState);
		}

		if (processResult != google_breakpad::PROCESS_OK) {
			AcceleratorConsoleWarning("Presubmit failed locally: minidump processor returned %d", processResult);
			return kPRLocalError;
		}

		std::string os_short = "";
		std::string cpu_arch = "";
		if (processState.system_info()) {
			os_short = processState.system_info()->os_short;
			if (os_short.empty()) {
				os_short = processState.system_info()->os;
			}
			cpu_arch = processState.system_info()->cpu;
		}

		int requestingThread = processState.requesting_thread();
		if (requestingThread == -1) {
			requestingThread = 0;
		}

		const google_breakpad::CallStack *stack = processState.threads()->at(requestingThread);
		if (!stack) {
			AcceleratorConsoleWarning("Presubmit failed locally: crashed thread stack was not available");
			return kPRLocalError;
		}

		int frameCount = stack->frames()->size();
		if (frameCount > 1024) {
			frameCount = 1024;
		}

		std::ostringstream summaryStream;
		summaryStream << 2 << "|" << processState.time_date_stamp() << "|" << os_short << "|" << cpu_arch << "|" << processState.crashed() << "|" << processState.crash_reason() << "|" << std::hex << processState.crash_address() << std::dec << "|" << requestingThread;

		std::map<const google_breakpad::CodeModule *, unsigned int> moduleMap;

		unsigned int moduleCount = processState.modules() ? processState.modules()->module_count() : 0;
		for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
			auto module = processState.modules()->GetModuleAtIndex(moduleIndex);
			moduleMap[module] = moduleIndex;

			auto debugFile = google_breakpad::PathnameStripper::File(module->debug_file());
			auto debugIdentifier = module->debug_identifier();

			summaryStream << "|M|" << debugFile << "|" << debugIdentifier;
		}

		for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
			auto frame = stack->frames()->at(frameIndex);

			int moduleIndex = -1;
			auto moduleOffset = frame->ReturnAddress();
			if (frame->module) {
				moduleIndex = moduleMap[frame->module];
				moduleOffset -= frame->module->base_address();
			}

			summaryStream << "|F|" << moduleIndex << "|" << std::hex << moduleOffset << std::dec;
		}

		auto summaryLine = summaryStream.str();
		// printf("%s\n", summaryLine.c_str());

		IWebForm *form = webternet->CreateForm();

		const char *minidumpAccount = g_pSM->GetCoreConfigValue("MinidumpAccount");
		if (minidumpAccount && minidumpAccount[0]) form->AddString("UserID", minidumpAccount);

		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);
		form->AddString("ServerID", serverId);

		form->AddString("CrashSignature", summaryLine.c_str());

		MemoryDownloader data;
		IWebTransfer *xfer = webternet->CreateSession();
		xfer->SetFailOnHTTPError(true);

		const char *minidumpUrl = g_pSM->GetCoreConfigValue("MinidumpUrl");
		if (!minidumpUrl) minidumpUrl = "http://crash.limetech.org/submit";

		auto watchdog = StartUploadWatchdog("presubmit", minidumpUrl);
		bool uploaded = xfer->PostAndDownload(minidumpUrl, form, &data, NULL);
		FinishUploadWatchdog(watchdog);

		if (!uploaded) {
			AcceleratorConsoleWarning("Presubmit failed via %s: %s (%d)",
				RedactUrlForLog(minidumpUrl).c_str(), xfer->LastErrorMessage(), xfer->LastErrorCode());
			AcceleratorDebugLog("Presubmit failed: %s (%d)", xfer->LastErrorMessage(), xfer->LastErrorCode());
			return kPRRemoteError;
		}

		int responseSize = data.GetSize();
		char *response = new char[responseSize + 1];
		strncpy(response, data.GetBuffer(), responseSize + 1);
		response[responseSize] = '\0';
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		//if (log) fprintf(log, "Presubmit complete: %s\n", response);

		if (responseSize < 2) {
			AcceleratorConsoleWarning("Presubmit failed: server response was too short from %s", RedactUrlForLog(minidumpUrl).c_str());
			AcceleratorDebugLog("Presubmit response too short");
			delete[] response;
			return kPRRemoteError;
		}

		if (response[0] == 'E') {
			AcceleratorConsoleWarning("Presubmit rejected by server %s: %s", RedactUrlForLog(minidumpUrl).c_str(), &response[2]);
			AcceleratorDebugLog("Presubmit error: %s", &response[2]);
			delete[] response;
			return kPRRemoteError;
		}

		PresubmitResponse presubmitResponse = kPRRemoteError;
		if (response[0] == 'Y') presubmitResponse = kPRUploadCrashDumpAndMetadata;
		else if (response[0] == 'N') presubmitResponse = kPRDontUpload;
		else if (response[0] == 'M') presubmitResponse = kPRUploadMetadataOnly;
		else return kPRRemoteError;

		if (response[1] != '|') {
			AcceleratorConsoleWarning("Presubmit failed: server response delimiter missing from %s", RedactUrlForLog(minidumpUrl).c_str());
			AcceleratorDebugLog("Response delimiter missing");
			delete[] response;
			return kPRRemoteError;
		}

		unsigned int responseCount = responseSize - 2;
		if (responseCount < moduleCount) {
			AcceleratorConsoleWarning("Presubmit warning: server module response was shorter than request (%u < %u)", responseCount, moduleCount);
			AcceleratorDebugLog("Response module list doesn't match sent list (%u < %u)", responseCount, moduleCount);
			delete[] response;
			return presubmitResponse;
		}

		// There was a presubmit token included.
		if (tokenBuffer && responseCount > moduleCount && response[2 + moduleCount] == '|') {
			int tokenStart = 2 + moduleCount + 1;
			int tokenEnd = tokenStart;
			while (tokenEnd < responseSize && response[tokenEnd] != '|') {
				tokenEnd++;
			}

			size_t tokenLength = tokenEnd - tokenStart;
			if (tokenLength < tokenBufferLength) {
				strncpy(tokenBuffer, &response[tokenStart], tokenLength);
				tokenBuffer[tokenLength] = '\0';
			}

			AcceleratorDebugLog("Got a presubmit token from server: %s", tokenBuffer);
		}

		if (moduleCount > 0) {
			auto mainModule = processState.modules()->GetMainModule();
			auto executableBaseDir = PathnameStripper_Directory(mainModule->code_file());
			InitModuleClassificationMap(executableBaseDir);

			// 0 = Disabled
			// 1 = System Only
			// 2 = System + Game
			// 3 = System + Game + Addons
			const char *symbolSubmitOptionStr = g_pSM->GetCoreConfigValue("MinidumpSymbolUpload");
			int symbolSubmitOption = symbolSubmitOptionStr ? atoi(symbolSubmitOptionStr) : 3;

			const char *binarySubmitOption = g_pSM->GetCoreConfigValue("MinidumpBinaryUpload");
			bool canBinarySubmit = !binarySubmitOption || (tolower(binarySubmitOption[0]) == 'y' || binarySubmitOption[0] == '1');

			for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
				bool submitSymbols = false;
				bool submitBinary = (response[2 + moduleIndex] == 'U');

#if defined _LINUX
				submitSymbols = (response[2 + moduleIndex] == 'Y');
#endif

				if (!submitSymbols && !submitBinary) {
					continue;
				}
				AcceleratorDebugLog("Getting module at index %d", moduleIndex);

				auto module = processState.modules()->GetModuleAtIndex(moduleIndex);

				auto moduleType = ClassifyModule(module);
				AcceleratorDebugLog("Classified module %s as %s", module->code_file().c_str(), ModuleTypeCode[moduleType]);
				switch (moduleType) {
					case kMTUnknown:
						continue;
					case kMTSystem:
						if (symbolSubmitOption < 1) {
							continue;
						}
						break;
					case kMTGame:
						if (symbolSubmitOption < 2) {
							continue;
						}
						break;
					case kMTAddon:
					case kMTExtension:
						if (symbolSubmitOption < 3) {
							continue;
						}
						break;
				}

				if (canBinarySubmit && submitBinary) {
					UploadModuleFile(module, tokenBuffer);
				}

#if defined _LINUX
				if (submitSymbols) {
					UploadSymbolFile(module, tokenBuffer);
				}
#endif
			}
		}
		AcceleratorDebugLog("PresubmitCrashDump complete");

		delete[] response;
		return presubmitResponse;
	}

	bool UploadCrashDump(const char *path, const char *metapath, const char *presubmitToken, char *response, int maxlen) {
		IWebForm *form = webternet->CreateForm();

		const char *minidumpAccount = g_pSM->GetCoreConfigValue("MinidumpAccount");
		if (minidumpAccount && minidumpAccount[0]) form->AddString("UserID", minidumpAccount);

		form->AddString("GameDirectory", crashGameDirectory);
		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		if (path && path[0]) {
			form->AddFile("upload_file_minidump", path);
		}

		if (metapath && metapath[0]) {
			form->AddFile("upload_file_metadata", metapath);
		}

		MemoryDownloader data;
		IWebTransfer *xfer = webternet->CreateSession();
		xfer->SetFailOnHTTPError(true);

		const char *minidumpUrl = g_pSM->GetCoreConfigValue("MinidumpUrl");
		if (!minidumpUrl) minidumpUrl = "http://crash.limetech.org/submit";

		auto watchdog = StartUploadWatchdog("crash dump upload", minidumpUrl);
		bool uploaded = xfer->PostAndDownload(minidumpUrl, form, &data, NULL);
		FinishUploadWatchdog(watchdog);

		if (response) {
			if (uploaded) {
				int responseSize = data.GetSize();
				if (responseSize >= maxlen) responseSize = maxlen - 1;
				strncpy(response, data.GetBuffer(), responseSize);
				response[responseSize] = '\0';
				while (responseSize > 0 && response[responseSize - 1] == '\n') {
					response[--responseSize] = '\0';
				}
			} else {
				g_pSM->Format(response, maxlen, "%s (%d)", xfer->LastErrorMessage(), xfer->LastErrorCode());
			}
		}

		if (!uploaded) {
			AcceleratorConsoleWarning("Crash dump upload failed via %s: %s (%d)",
				RedactUrlForLog(minidumpUrl).c_str(), xfer->LastErrorMessage(), xfer->LastErrorCode());
		}

		return uploaded;
	}
} uploadThread;

class SourcePawnNotifyThread : public IThread
{
public:

	void RunThread(IThreadHandle* pHandle) {
		for (;;) {
			// Wait until OnMapStart is called once, this should be enough delay to make sure plugins are loaded.
			if (g_accelerator.IsMapStarted() && g_accelerator.IsDoneUploading()) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	void OnTerminate(IThreadHandle* pHandle, bool cancel) {
	}

} spNotifyThread;

static void StartAcceleratorBackgroundThreads()
{
	if (acceleratorBackgroundThreadsStarted.exchange(true, std::memory_order_relaxed)) {
		return;
	}

	const bool localMode = IsLocalMode();
	if (!localMode) {
		threader->MakeThread(&uploadThread);
	} else {
		g_accelerator.MarkAsDoneUploading();
		StartLocalDumpWorker();
	}

	// This thread waits for Accelerator to finish uploading and for the first OnMapStart call,
	// then fires a SourceMod forward.
	threader->MakeThread(&spNotifyThread);
}

class VFuncEmptyClass {};

static const char *GetProcessCommandLine()
{
	static char cmdline[1024] = {0};
	if (cmdline[0]) {
		return cmdline;
	}

#if defined _LINUX
	FILE *fp = fopen("/proc/self/cmdline", "rb");
	if (!fp) {
		return "";
	}

	size_t bytes = fread(cmdline, 1, sizeof(cmdline) - 1, fp);
	fclose(fp);

	for (size_t i = 0; i < bytes; ++i) {
		if (cmdline[i] == '\0') {
			cmdline[i] = ' ';
		}
	}
	cmdline[bytes] = '\0';
#elif defined _WINDOWS
	const char *raw = GetCommandLineA();
	if (!raw || !raw[0]) {
		return "";
	}
	strncpy(cmdline, raw, sizeof(cmdline) - 1);
#else
	return "";
#endif

	return cmdline;
}

const char *GetCmdLine()
{
#if !defined SMEXT_ENABLE_GAMEHELPERS
	return GetProcessCommandLine();
#else
	static int getCmdLineOffset = 0;
	if (getCmdLineOffset == 0) {
		if (!gameconfig || !gameconfig->GetOffset("GetCmdLine", &getCmdLineOffset)) {
			return "";
		}
		if (getCmdLineOffset == 0) {
			return "";
		}
	}

	void *cmdline = nullptr;
#if defined SMEXT_ENABLE_GAMEHELPERS
	cmdline = gamehelpers->GetValveCommandLine();
#endif
	if (!cmdline) {
		return GetProcessCommandLine();
	}
	void **vtable = *(void ***)cmdline;
	void *vfunc = vtable[getCmdLineOffset];

	union {
		const char *(VFuncEmptyClass::*mfpnew)();
#ifndef WIN32
		struct {
			void *addr;
			intptr_t adjustor;
		} s;
	} u;
	u.s.addr = vfunc;
	u.s.adjustor = 0;
#else
		void *addr;
	} u;
	u.addr = vfunc;
#endif

	return (const char *)(reinterpret_cast<VFuncEmptyClass*>(cmdline)->*u.mfpnew)();
#endif
}

Accelerator::Accelerator() :
	m_doneuploading(false), m_maphasstarted(false)
{
}

bool Accelerator::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	acceleratorDebugLoggingEnabled.store(IsTruthyCoreConfigValue("MinidumpDebug", false), std::memory_order_relaxed);
	const bool localMode = IsLocalMode();
	if (!localMode) {
		sharesys->AddDependency(myself, "webternet.ext", true, true);
		SM_GET_IFACE(WEBTERNET, webternet);
	} else {
		webternet = nullptr;
	}

	g_pSM->BuildPath(Path_SM, dumpStoragePath, sizeof(dumpStoragePath), "data/dumps");

	if (!libsys->IsPathDirectory(dumpStoragePath))
	{
		if (!libsys->CreateFolder(dumpStoragePath))
		{
			if (error)
				g_pSM->Format(error, maxlength, "%s didn't exist and we couldn't create it :(", dumpStoragePath);
			return false;
		}
	}

	g_pSM->BuildPath(Path_SM, logPath, sizeof(logPath), "logs/accelerator.log");

	// Get these early so path resolution and background workers can use them safely.
	strncpy(crashGamePath, g_pSM->GetGamePath(), sizeof(crashGamePath) - 1);
	strncpy(crashSourceModPath, g_pSM->GetSourceModPath(), sizeof(crashSourceModPath) - 1);
	strncpy(crashGameDirectory, g_pSM->GetGameFolderName(), sizeof(crashGameDirectory) - 1);

	AcceleratorConsoleMessage("Accelerator mode: %s", localMode ? "local" : "site");
	AcceleratorDebugLog("Accelerator mode: %s", localMode ? "local" : "site");
	if (localMode) {
		const std::string carburetorPath = GetConfiguredCarburetorPath();
		const std::vector<std::string> symbolPaths = GetConfiguredLocalSymbolPaths();
		const std::string joinedSymbolPaths = symbolPaths.empty() ? "(none)" : JoinStrings(symbolPaths, "; ");
		std::string createError;
		if (!EnsureDirectoryExists(GetLocalSymbolStoreRoot(), createError) && !createError.empty()) {
			AcceleratorConsoleWarning("Failed to create local symbol store root: %s", createError.c_str());
		}
		createError.clear();
		if (!EnsureDirectoryExists(GetLocalOutputRoot(), createError) && !createError.empty()) {
			AcceleratorConsoleWarning("Failed to create local output root: %s", createError.c_str());
		}
		AcceleratorDebugLog("Local mode carburetor path: %s", carburetorPath.c_str());
		AcceleratorDebugLog("Local mode symbol paths: %s", joinedSymbolPaths.c_str());
		if (!PathExistsFile(carburetorPath)) {
			AcceleratorConsoleWarning("Carburetor binary is missing. rawraw will fall back to built-in output until MinidumpLocalCarburetorPath is configured.");
		}
	}

	do {
		char gameconfigError[256];
		if (!gameconfs->LoadGameConfigFile("accelerator.games", &gameconfig, gameconfigError, sizeof(gameconfigError))) {
			smutils->LogMessage(myself, "WARNING: Failed to load gamedata file, console output and command line will not be included in crash reports: %s", gameconfigError);
			break;
		}

		bool useFastcall = false;


#if defined _WINDOWS
		const char *fastcall = gameconfig->GetKeyValue("UseFastcall");
		if (fastcall && strcmp(fastcall, "yes") == 0) {
			useFastcall = true;
		}

		if (useFastcall && !gameconfig->GetMemSig("GetSpewFastcall", (void **)&GetSpewFastcall)) {
			smutils->LogMessage(myself, "WARNING: GetSpewFastcall not found in gamedata, console output will not be included in crash reports.");
			break;
		}
#endif

		if (!useFastcall && !gameconfig->GetMemSig("GetSpew", (void **)&GetSpew)) {
			smutils->LogMessage(myself, "WARNING: GetSpew not found in gamedata, console output will not be included in crash reports.");
			break;
		}

		if (!GetSpew
#if defined _WINDOWS
			&& !GetSpewFastcall
#endif
		) {
			smutils->LogMessage(myself, "WARNING: Sigscan for GetSpew failed, console output will not be included in crash reports.");
			break;
		}
	} while(false);

#if defined _LINUX
	google_breakpad::MinidumpDescriptor descriptor(dumpStoragePath);
	handler = new google_breakpad::ExceptionHandler(descriptor, NULL, dumpCallback, NULL, true, -1);

	struct sigaction oact;
	sigaction(SIGSEGV, NULL, &oact);
	SignalHandler = oact.sa_sigaction;

	g_pSM->AddGameFrameHook(OnGameFrame);
#elif defined _WINDOWS
	wchar_t *buf = new wchar_t[sizeof(dumpStoragePath)];
	size_t num_chars = mbstowcs(buf, dumpStoragePath, sizeof(dumpStoragePath));

	handler = new google_breakpad::ExceptionHandler(
		std::wstring(buf, num_chars), NULL, dumpCallback, NULL, google_breakpad::ExceptionHandler::HANDLER_ALL,
		static_cast<MINIDUMP_TYPE>(MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo), static_cast<const wchar_t *>(NULL), NULL);

	vectoredHandler = AddVectoredExceptionHandler(0, BreakpadVectoredHandler);

	delete buf;
#else
#error Bad platform.
#endif

	#if SMINTERFACE_EXTENSIONAPI_VERSION >= 9
	strncpy(crashSourceModVersion, SM_FULL_VERSION, sizeof(crashSourceModVersion) - 1);
	crashSourceModVersion[sizeof(crashSourceModVersion) - 1] = '\0';
	#else
	do {
		char spJitPath[512];
		g_pSM->BuildPath(Path_SM, spJitPath, sizeof(spJitPath), "bin/" PLATFORM_ARCH_FOLDER "sourcepawn.jit.x86." PLATFORM_LIB_EXT);

		char spJitError[255];
		std::unique_ptr<ILibrary> spJit(libsys->OpenLibrary(spJitPath, spJitError, sizeof(spJitError)));
		if (!spJit) {
			smutils->LogMessage(myself, "WARNING: Failed to load SourcePawn library %s: %s", spJitPath, spJitError);
			break;
		}

		GetSourcePawnFactoryFn factoryFn = (GetSourcePawnFactoryFn)spJit->GetSymbolAddress("GetSourcePawnFactory");
		if (!factoryFn) {
			smutils->LogMessage(myself, "WARNING: SourcePawn library is out of date: No factory function.");
			break;
		}

		ISourcePawnFactory *spFactory = factoryFn(0x0207);
		if (!spFactory) {
			smutils->LogMessage(myself, "WARNING: SourcePawn library is out of date: Failed to get version 2.7", 0x0207);
			break;
		}

		ISourcePawnEnvironment *spEnvironment = spFactory->CurrentEnvironment();
		if (!spEnvironment) {
			smutils->LogMessage(myself, "WARNING: Could not get SourcePawn environment.");
			break;
		}

		ISourcePawnEngine2 *spEngine2 = spEnvironment->APIv2();
		if (!spEngine2) {
			smutils->LogMessage(myself, "WARNING: Could not get SourcePawn engine2.");
			break;
		}

		strncpy(crashSourceModVersion, spEngine2->GetVersionString(), sizeof(crashSourceModVersion));
	} while(false);
	#endif

	plsys->AddPluginsListener(this);

	IPluginIterator *iterator = plsys->GetPluginIterator();
	while (iterator->MorePlugins()) {
		IPlugin *plugin = iterator->GetPlugin();
		if (plugin->GetStatus() == Plugin_Running) {
			this->OnPluginLoaded(plugin);
		}
		iterator->NextPlugin();
	}
	delete iterator;

	strncpy(crashCommandLine, GetCmdLine(), sizeof(crashCommandLine) - 1);

	char steamInfPath[512];
	g_pSM->BuildPath(Path_Game, steamInfPath, sizeof(steamInfPath), "steam.inf");

	FILE *steamInfFile = fopen(steamInfPath, "rb");
	if (steamInfFile) {
		char steamInfTemp[1024] = {0};
		fread(steamInfTemp, sizeof(char), sizeof(steamInfTemp) - 1, steamInfFile);

		fclose(steamInfFile);

		unsigned commentChars = 0;
		unsigned valueChars = 0;
		unsigned source = 0;
		strcpy(steamInf, "\nSteam_");
		unsigned target = 7; // strlen("\nSteam_");
		while (true) {
			if (steamInfTemp[source] == '\0') {
				source++;
				break;
			}
			if (steamInfTemp[source] == '/') {
				source++;
				commentChars++;
				continue;
			}
			if (commentChars == 1) {
				commentChars = 0;
				steamInf[target++] = '/';
				valueChars++;
			}
			if (steamInfTemp[source] == '\r') {
				source++;
				continue;
			}
			if (steamInfTemp[source] == '\n') {
				commentChars = 0;
				source++;
				if (steamInfTemp[source] == '\0') {
					break;
				}
				if (valueChars > 0) {
					valueChars = 0;
					strcpy(&steamInf[target], "\nSteam_");
					target += 7;
				}
				continue;
			}
			if (commentChars >= 2) {
				source++;
				continue;
			}
			steamInf[target++] = steamInfTemp[source++];
			valueChars++;
		}
	}

	if (late) {
		this->OnCoreMapStart(NULL, 0, 0);
	}

	return true;
}

void Accelerator::SDK_OnUnload()
{
	extforwards::Shutdown();
	plsys->RemovePluginsListener(this);
	if (IsLocalMode()) {
		StopLocalDumpWorker();
	}

#if defined _LINUX
	g_pSM->RemoveGameFrameHook(OnGameFrame);
#elif defined _WINDOWS
	if (vectoredHandler) {
		RemoveVectoredExceptionHandler(vectoredHandler);
	}
#else
#error Bad platform.
#endif

	delete handler;
}

void Accelerator::SDK_OnAllLoaded()
{
	acceleratorDebugLoggingEnabled.store(IsTruthyCoreConfigValue("MinidumpDebug", false), std::memory_order_relaxed);
	extforwards::Init();
	sharesys->RegisterLibrary(myself, "accelerator");

	natives::Setup(m_natives);
	m_natives.push_back({ nullptr, nullptr }); // SM requires this to signal the end of the native info array

	sharesys->AddNatives(myself, m_natives.data());
	StartAcceleratorBackgroundThreads();
}

void Accelerator::OnCoreMapStart(edict_t *pEdictList, int edictCount, int clientMax)
{
	crashMap[0] = '\0';
	m_maphasstarted.store(true);
}

/* 010 Editor Template
uint64 headerMagic;
uint32 version;
uint32 size;
uint32 count;
struct {
    uint32 size;
    uint32 context <format=hex>;
    char file[];
    uint32 count;
    struct {
        uint32 pcode <format=hex>;
        char name[];
    } functions[count] <optimize=false>;
} plugins[count] <optimize=false>;
uint64 tailMagic;
*/

unsigned char *serializedPluginContexts = nullptr;
std::map<const IPluginContext *, unsigned char *> pluginContextMap;

void SerializePluginContexts()
{
	if (serializedPluginContexts) {
		handler->UnregisterAppMemory(serializedPluginContexts);
		free(serializedPluginContexts);
		serializedPluginContexts = nullptr;
	}

	uint32_t count = pluginContextMap.size();
	if (count == 0) {
		return;
	}

	uint32_t size = 0;
	size += sizeof(uint64_t); // header magic
	size += sizeof(uint32_t); // version
	size += sizeof(uint32_t); // size
	size += sizeof(uint32_t); // count

	for (auto &it : pluginContextMap) {
		unsigned char *buffer = it.second;

		uint32_t bufferSize;
		memcpy(&bufferSize, buffer, sizeof(uint32_t));

		size += bufferSize;
	}

	size += sizeof(uint64_t); // tail magic

	serializedPluginContexts = (unsigned char *)malloc(size);
	handler->RegisterAppMemory(serializedPluginContexts, size);
	unsigned char *cursor = serializedPluginContexts;

	uint64_t headerMagic = 103582791429521979ULL;
	memcpy(cursor, &headerMagic, sizeof(uint64_t));
	cursor += sizeof(uint64_t);

	uint32_t version = 1;
	memcpy(cursor, &version, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	memcpy(cursor, &size, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	memcpy(cursor, &count, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	for (auto &it : pluginContextMap) {
		unsigned char *buffer = it.second;

		uint32_t bufferSize;
		memcpy(&bufferSize, buffer, sizeof(uint32_t));

		memcpy(cursor, buffer, bufferSize);
		cursor += bufferSize;
	}

	uint64_t tailMagic = 76561197987819599ULL;
	memcpy(cursor, &tailMagic, sizeof(uint64_t));
	cursor += sizeof(uint64_t);
}

void Accelerator::OnPluginLoaded(IPlugin *plugin)
{
	IPluginRuntime *runtime = plugin->GetRuntime();
	IPluginContext *context = plugin->GetBaseContext();
	if (!runtime || !context) {
		return;
	}

	const char *filename = plugin->GetFilename();
	size_t filenameSize = strlen(filename) + 1;

	uint32_t size = 0;
	size += sizeof(uint32_t); // size
	size += sizeof(void *); // GetBaseContext
	size += filenameSize;

	uint32_t count = 0;
#if SMINTERFACE_EXTENSIONAPI_VERSION < 9
	count = runtime->GetPublicsNum();
#endif
	size += sizeof(uint32_t); // count
	size += count * sizeof(uint32_t); // pubinfo->code_offs

#if SMINTERFACE_EXTENSIONAPI_VERSION < 9
	for (uint32_t i = 0; i < count; ++i) {
		sp_public_t *pubinfo;
		runtime->GetPublicByIndex(i, &pubinfo);

		size += strlen(pubinfo->name) + 1;
	}
#endif

	unsigned char *buffer = (unsigned char *)malloc(size);
	unsigned char *cursor = buffer;

	memcpy(cursor, &size, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	memcpy(cursor, &context, sizeof(void *));
	cursor += sizeof(void *);

	memcpy(cursor, filename, filenameSize);
	cursor += filenameSize;

	memcpy(cursor, &count, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

#if SMINTERFACE_EXTENSIONAPI_VERSION < 9
	for (uint32_t i = 0; i < count; ++i) {
		sp_public_t *pubinfo;
		runtime->GetPublicByIndex(i, &pubinfo);

		memcpy(cursor, &pubinfo->code_offs, sizeof(uint32_t));
		cursor += sizeof(uint32_t);

		size_t nameSize = strlen(pubinfo->name) + 1;
		memcpy(cursor, pubinfo->name, nameSize);
		cursor += nameSize;
	}
#endif

	pluginContextMap[context] = buffer;

	SerializePluginContexts();
}

void Accelerator::OnPluginUnloaded(IPlugin *plugin)
{
	IPluginContext *context = plugin->GetBaseContext();
	if (!context) {
		return;
	}

	auto it = pluginContextMap.find(context);
	if (it != pluginContextMap.end()) {
		free(it->second);
		pluginContextMap.erase(it);
	}

	SerializePluginContexts();
}

void Accelerator::StoreUploadedCrash(UploadedCrash& crash)
{
	std::lock_guard<std::mutex> lock(m_uploadedcrashes_mutex);
	m_uploadedcrashes.push_back(std::move(crash));
}

const UploadedCrash* Accelerator::GetUploadedCrash(int element) const
{
	std::lock_guard<std::mutex> lock(m_uploadedcrashes_mutex);
	if (element < 0 || element >= static_cast<int>(m_uploadedcrashes.size())) {
		return nullptr;
	}

	return &m_uploadedcrashes[element];
}

cell_t Accelerator::GetUploadedCrashCount() const
{
	std::lock_guard<std::mutex> lock(m_uploadedcrashes_mutex);
	return static_cast<cell_t>(m_uploadedcrashes.size());
}
