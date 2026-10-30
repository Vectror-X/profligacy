// SPDX-License-Identifier: AGPL-3.0-only
#include "engine_link_platform.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#if defined(_MSC_VER)
#pragma comment(lib, "winmm.lib")
#endif
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace enginelink {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string &s)
{
	if (s.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
	std::wstring w((std::size_t) n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
	return w;
}

std::string narrow(const std::wstring &w)
{
	if (w.empty()) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((std::size_t) n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

// One job per host process, never closed: when the host exits (or crashes) the
// kernel closes it and every helper still inside is terminated.
HANDLE hostJob()
{
	static HANDLE job = [] {
		HANDLE j = CreateJobObjectW(nullptr, nullptr);
		if (j != nullptr)
		{
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
			info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info));
		}
		return j;
	}();
	return job;
}

std::wstring quoteArg(const std::wstring &arg)
{
	if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
	std::wstring out = L"\"";
	std::size_t backslashes = 0;
	for (wchar_t c : arg)
	{
		if (c == L'\\') { ++backslashes; continue; }
		if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
		else out.append(backslashes, L'\\');
		backslashes = 0;
		out.push_back(c);
	}
	out.append(backslashes * 2, L'\\');
	out.push_back(L'"');
	return out;
}

} // namespace

SharedRegion::~SharedRegion() { close(); }

bool SharedRegion::create(const std::string &name, std::size_t size)
{
	close();
	const auto wname = widen(name);
	HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		(DWORD) ((unsigned long long) size >> 32), (DWORD) (size & 0xffffffffu), wname.c_str());
	if (h == nullptr) return false;
	if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(h); return false; }
	void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
	if (p == nullptr) { CloseHandle(h); return false; }
	m_handle = h; m_data = p; m_size = size; m_name = name; m_owner = true;
	return true;
}

bool SharedRegion::open(const std::string &name, std::size_t size)
{
	close();
	const auto wname = widen(name);
	HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
	if (h == nullptr) return false;
	void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
	if (p == nullptr) { CloseHandle(h); return false; }
	m_handle = h; m_data = p; m_size = size; m_name = name; m_owner = false;
	return true;
}

void SharedRegion::close()
{
	if (m_data != nullptr) UnmapViewOfFile(m_data);
	if (m_handle != nullptr) CloseHandle((HANDLE) m_handle);
	m_data = nullptr; m_handle = nullptr; m_size = 0; m_owner = false;
}

ChildProcess::~ChildProcess()
{
	if (m_process != nullptr) CloseHandle((HANDLE) m_process);
}

bool ChildProcess::spawn(const std::string &exe, const std::vector<std::string> &args)
{
	const std::wstring wexe = widen(exe);
	std::wstring cmd = quoteArg(wexe);
	for (const auto &a : args) { cmd += L' '; cmd += quoteArg(widen(a)); }
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(wexe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
		return false;
	if (HANDLE job = hostJob(); job != nullptr)
		(void) AssignProcessToJobObject(job, pi.hProcess); // best effort; the helper also watches its host
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	m_process = pi.hProcess;
	return true;
}

bool ChildProcess::started() const { return m_process != nullptr; }

bool ChildProcess::alive() const
{
	return m_process != nullptr && WaitForSingleObject((HANDLE) m_process, 0) == WAIT_TIMEOUT;
}

bool ChildProcess::waitForExit(int milliseconds) const
{
	return m_process == nullptr || WaitForSingleObject((HANDLE) m_process, (DWORD) milliseconds) == WAIT_OBJECT_0;
}

void ChildProcess::kill()
{
	if (alive()) TerminateProcess((HANDLE) m_process, 1);
}

std::uint64_t currentProcessId() { return GetCurrentProcessId(); }

bool processAlive(std::uint64_t pid)
{
	HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);
	if (h == nullptr) return false;
	const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
	CloseHandle(h);
	return alive;
}

std::string thisModuleDirectory()
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&thisModuleDirectory), &module))
		return {};
	std::wstring path(32768, L'\0');
	const DWORD n = GetModuleFileNameW(module, path.data(), (DWORD) path.size());
	if (n == 0) return {};
	path.resize(n);
	const auto slash = path.find_last_of(L"\\/");
	return narrow(slash == std::wstring::npos ? std::wstring() : path.substr(0, slash));
}

std::string makeRegionName(std::uint64_t serial)
{
	return "Local\\ProfligacyEngine-" + std::to_string(currentProcessId()) + "-" + std::to_string(serial)
		+ "-" + std::to_string(GetTickCount64());
}

void sleepMilliseconds(int ms) { Sleep((DWORD) ms); }

void helperProcessSetup()
{
	timeBeginPeriod(1);
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
}

void setEnvironment(const char *name, const char *value, bool overwrite)
{
	if (!overwrite && std::getenv(name) != nullptr) return;
	_putenv_s(name, value);
}

#else // POSIX (test builds)

SharedRegion::~SharedRegion() { close(); }

bool SharedRegion::create(const std::string &name, std::size_t size)
{
	close();
	const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0) return false;
	if (ftruncate(fd, (off_t) size) != 0) { ::close(fd); shm_unlink(name.c_str()); return false; }
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	::close(fd);
	if (p == MAP_FAILED) { shm_unlink(name.c_str()); return false; }
	m_data = p; m_size = size; m_name = name; m_owner = true;
	return true;
}

bool SharedRegion::open(const std::string &name, std::size_t size)
{
	close();
	const int fd = shm_open(name.c_str(), O_RDWR, 0600);
	if (fd < 0) return false;
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	::close(fd);
	if (p == MAP_FAILED) return false;
	m_data = p; m_size = size; m_name = name; m_owner = false;
	return true;
}

void SharedRegion::close()
{
	if (m_data != nullptr) munmap(m_data, m_size);
	if (m_owner) shm_unlink(m_name.c_str());
	m_data = nullptr; m_size = 0; m_owner = false;
}

ChildProcess::~ChildProcess()
{
	if (m_pid > 0) { int status = 0; (void) waitpid((pid_t) m_pid, &status, WNOHANG); }
}

bool ChildProcess::spawn(const std::string &exe, const std::vector<std::string> &args)
{
	std::vector<char *> argv;
	argv.push_back(const_cast<char *>(exe.c_str()));
	for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);
	pid_t pid = 0;
	if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return false;
	m_pid = pid;
	return true;
}

bool ChildProcess::started() const { return m_pid > 0; }

bool ChildProcess::alive() const
{
	if (m_pid <= 0) return false;
	int status = 0;
	const pid_t r = waitpid((pid_t) m_pid, &status, WNOHANG);
	if (r == (pid_t) m_pid) { const_cast<ChildProcess *>(this)->m_pid = -1; return false; }
	return r == 0;
}

bool ChildProcess::waitForExit(int milliseconds) const
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (alive())
	{
		if (std::chrono::steady_clock::now() >= end) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

void ChildProcess::kill()
{
	if (alive()) ::kill((pid_t) m_pid, SIGKILL);
	(void) waitForExit(1000);
}

std::uint64_t currentProcessId() { return (std::uint64_t) getpid(); }

bool processAlive(std::uint64_t pid) { return ::kill((pid_t) pid, 0) == 0; }

std::string thisModuleDirectory()
{
	Dl_info info{};
	if (dladdr(reinterpret_cast<void *>(&thisModuleDirectory), &info) == 0 || info.dli_fname == nullptr) return {};
	std::string path = info.dli_fname;
	const auto slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string makeRegionName(std::uint64_t serial)
{
	// macOS limits shared-memory names to 31 characters.
	return "/prfl" + std::to_string(currentProcessId() % 1000000) + "-" + std::to_string(serial % 10000)
		+ "-" + std::to_string((long long) (std::chrono::steady_clock::now().time_since_epoch().count() % 1000000LL));
}

void sleepMilliseconds(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void helperProcessSetup() {}

void setEnvironment(const char *name, const char *value, bool overwrite)
{
	setenv(name, value, overwrite ? 1 : 0);
}

#endif

} // namespace enginelink
