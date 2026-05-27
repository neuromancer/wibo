#include "msvcrt.h"

#include "kernel32/fileapi.h"
#include "kernel32/heapapi.h"
#include "kernel32/internal.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "macros.h"
#include "modules.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <random>
#include <strings.h>
#include <string>
#include <system_error>

#include "msvcrt_trampolines.h"

namespace {

using random_words_engine =
	std::independent_bits_engine<std::default_random_engine, sizeof(uint32_t) * 8, uint32_t>;

constexpr char kTempAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
constexpr char kDefaultPrefix[] = "tmp";
constexpr char kTempSuffixTemplate[] = "XXXXXX";
constexpr size_t kTempnamPrefixLimit = 5;
constexpr size_t kMktempAttemptLimit = 4096;
constexpr uint16_t kSopenOrdinal = 755;
constexpr uint16_t kStatOrdinal = 772;
constexpr uint16_t kStrdupOrdinal = 780;

std::string trimTrailingSeparators(std::string path) {
	while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
		path.pop_back();
	}
	return path;
}

bool windowsPathExists(LPCSTR windowsPath) {
	if (!windowsPath || !*windowsPath) {
		return false;
	}
	std::error_code ec;
	return std::filesystem::exists(files::pathFromWindows(windowsPath), ec);
}

bool isGeneratedTempFile(LPCSTR filename) {
	if (!filename) {
		return false;
	}
	size_t length = std::strlen(filename);
	return length >= 4 && strcasecmp(filename + length - 4, ".tmp") == 0;
}

std::string normalizeTempDirectory(LPCSTR dir) {
	if (dir && *dir) {
		std::filesystem::path hostPath = files::pathFromWindows(dir);
		std::error_code ec;
		if (std::filesystem::exists(hostPath, ec) && std::filesystem::is_directory(hostPath, ec)) {
			return trimTrailingSeparators(files::pathToWindows(files::canonicalPath(hostPath)));
		}
	}

	std::array<char, MAX_PATH> tempPath{};
	DWORD length = kernel32::GetTempPathA(static_cast<DWORD>(tempPath.size()), tempPath.data());
	if (length > 0 && length < tempPath.size()) {
		return trimTrailingSeparators(std::string(tempPath.data(), length));
	}
	return "Z:\\tmp";
}

std::string normalizeTempPrefix(LPCSTR prefix) {
	if (!prefix || !*prefix) {
		return kDefaultPrefix;
	}
	std::string value(prefix, std::min(std::strlen(prefix), kTempnamPrefixLimit));
	for (char &ch : value) {
		if (ch == '\\' || ch == '/' || ch == ':') {
			ch = '_';
		}
	}
	return value;
}

} // namespace

INCLUDE_BIN(_msvcrtDllData, EMBED_PATH)

namespace msvcrt {

LPSTR CDECL _mktemp(LPSTR templateName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("_mktemp(%s)\n", templateName ? templateName : "(null)");
	if (!templateName) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}

	size_t length = std::strlen(templateName);
	char *suffixStart = templateName + length;
	while (suffixStart > templateName && suffixStart[-1] == 'X') {
		--suffixStart;
	}
	size_t suffixLength = static_cast<size_t>((templateName + length) - suffixStart);
	if (suffixLength == 0) {
		templateName[0] = '\0';
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}

	std::random_device rd;
	random_words_engine random(rd());
	constexpr uint64_t kAlphabetSize = sizeof(kTempAlphabet) - 1;
	for (size_t attempt = 0; attempt < kMktempAttemptLimit; ++attempt) {
		uint64_t value = (static_cast<uint64_t>(random()) << 32) ^ random();
		value ^= static_cast<uint64_t>(attempt) * 0x9E3779B97F4A7C15ull;
		for (size_t i = 0; i < suffixLength; ++i) {
			suffixStart[suffixLength - 1 - i] = kTempAlphabet[value % kAlphabetSize];
			value /= kAlphabetSize;
		}
		if (!windowsPathExists(templateName)) {
			kernel32::setLastError(ERROR_SUCCESS);
			DEBUG_LOG(" -> %s\n", templateName);
			return templateName;
		}
	}

	templateName[0] = '\0';
	kernel32::setLastError(ERROR_FILE_EXISTS);
	return nullptr;
}

LPSTR CDECL _tempnam(LPCSTR dir, LPCSTR prefix) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("_tempnam(%s, %s)\n", dir ? dir : "(null)", prefix ? prefix : "(null)");

	std::string candidate = normalizeTempDirectory(dir);
	if (!candidate.empty()) {
		candidate.push_back('\\');
	}
	candidate += normalizeTempPrefix(prefix);
	candidate += kTempSuffixTemplate;

	// _tempnam must hand back an unused name; the caller creates the file later.
	if (!_mktemp(candidate.data())) {
		return nullptr;
	}

	SIZE_T bytes = candidate.size() + 1;
	auto *buffer = static_cast<char *>(kernel32::HeapAlloc(kernel32::GetProcessHeap(), 0, bytes));
	if (!buffer) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return nullptr;
	}
	std::memcpy(buffer, candidate.c_str(), bytes);
	kernel32::setLastError(ERROR_SUCCESS);
	DEBUG_LOG(" -> %s\n", buffer);
	return buffer;
}

LPSTR CDECL _strdup(LPCSTR string) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("_strdup(ptr=%p, str=%s)\n", string, string ? string : "(null)");

	auto *module = wibo::findLoadedModule("msvcrt");
	auto original = module ? reinterpret_cast<MSVCRT_STRDUP>(wibo::resolveFuncByOrdinal(module, kStrdupOrdinal)) : nullptr;
	if (!original) {
		kernel32::setLastError(ERROR_PROC_NOT_FOUND);
		return nullptr;
	}

	LPSTR result = call_MSVCRT_STRDUP(original, string);
	DEBUG_LOG(" -> %p (%s, lastError=%u)\n", result, result ? result : "(null)", kernel32::getLastError());
	return result;
}

int CDECL _sopen(LPCSTR filename, int oflag, int shflag, int pmode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("_sopen(ptr=%p, str=%s, 0x%x, 0x%x, 0x%x)\n", filename, filename ? filename : "(null)", oflag,
			  shflag, pmode);

	auto *module = wibo::findLoadedModule("msvcrt");
	auto original = module ? reinterpret_cast<MSVCRT_SOPEN>(wibo::resolveFuncByOrdinal(module, kSopenOrdinal)) : nullptr;
	if (!original) {
		kernel32::setLastError(ERROR_PROC_NOT_FOUND);
		return -1;
	}

	int fd = call_MSVCRT_SOPEN(original, filename, oflag, shflag, pmode);
	if (fd != -1) {
		DEBUG_LOG(" -> %d\n", fd);
		return fd;
	}

	DWORD lastError = kernel32::getLastError();
	DEBUG_LOG(" -> -1 (lastError=%u)\n", lastError);

	if (isGeneratedTempFile(filename) && windowsPathExists(filename)) {
		DEBUG_LOG("  retrying temp open after removing precreated file\n");
		kernel32::DeleteFileA(filename);
		fd = call_MSVCRT_SOPEN(original, filename, oflag, shflag, pmode);
		DEBUG_LOG("  retry -> %d (lastError=%u)\n", fd, kernel32::getLastError());
		return fd;
	}

	return -1;
}

int CDECL _stat(LPCSTR path, void *buffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("_stat(ptr=%p, str=%s, %p)\n", path, path ? path : "(null)", buffer);

	auto *module = wibo::findLoadedModule("msvcrt");
	auto original = module ? reinterpret_cast<MSVCRT_STAT>(wibo::resolveFuncByOrdinal(module, kStatOrdinal)) : nullptr;
	if (!original) {
		kernel32::setLastError(ERROR_PROC_NOT_FOUND);
		return -1;
	}

	int result = call_MSVCRT_STAT(original, path, buffer);
	DEBUG_LOG(" -> %d (lastError=%u)\n", result, kernel32::getLastError());
	return result;
}

} // namespace msvcrt

extern const wibo::ModuleStub lib_msvcrt = {
	.names =
		(const char *[]){
			"msvcrt",
			nullptr,
		},
	.byName = msvcrtThunkByName,
	.dllData = INCLUDE_BIN_SPAN(_msvcrtDllData),
};
