// SPDX-License-Identifier: AGPL-3.0-only
//
// engine_link_platform.h - the few operating-system pieces the multi-instance
// link needs: a named shared-memory region, a child process, and small helpers.
// Windows is the target; the POSIX version exists so the protocol can be tested
// on any machine.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace enginelink {

class SharedRegion
{
public:
	SharedRegion() = default;
	~SharedRegion();
	SharedRegion(const SharedRegion &) = delete;
	SharedRegion &operator=(const SharedRegion &) = delete;

	bool create(const std::string &name, std::size_t size); // zero-filled
	bool open(const std::string &name, std::size_t size);
	void close();
	void *data() const { return m_data; }

private:
	void *m_data = nullptr;
	std::size_t m_size = 0;
	std::string m_name;
	bool m_owner = false;
	void *m_handle = nullptr; // Windows mapping handle
};

class ChildProcess
{
public:
	ChildProcess() = default;
	~ChildProcess();
	ChildProcess(const ChildProcess &) = delete;
	ChildProcess &operator=(const ChildProcess &) = delete;

	// Start `exe` with `args` (argv[1..]). On Windows the child gets no console
	// window and joins a job that kills it if the host process disappears.
	bool spawn(const std::string &exe, const std::vector<std::string> &args);
	bool alive() const;
	bool waitForExit(int milliseconds) const;
	void kill();
	bool started() const;

private:
	void *m_process = nullptr; // Windows HANDLE
	long long m_pid = 0;       // POSIX pid
};

std::uint64_t currentProcessId();
bool processAlive(std::uint64_t pid);
std::string thisModuleDirectory();      // folder of the binary that contains this code
std::string makeRegionName(std::uint64_t serial);
void sleepMilliseconds(int ms);
void helperProcessSetup();              // 1 ms timer resolution, above-normal priority
void setEnvironment(const char *name, const char *value, bool overwrite);

} // namespace enginelink
