// SPDX-License-Identifier: AGPL-3.0-only
#include "engine_facade.h"

#include <cstdlib>

std::atomic<bool> EngineFacade::s_inProcessTaken{false};

EngineFacade::~EngineFacade()
{
	stop();
	releaseReservation();
}

bool EngineFacade::reserveInProcess()
{
	if (m_decided) return m_reservedInProcess;
	m_decided = true;
	// PROFLIGACY_SINGLE_INSTANCE=1 restores the original behaviour (no helper:
	// extra instances stay silent), in case a host or machine dislikes child processes.
	const bool helpersAllowed = std::getenv("PROFLIGACY_SINGLE_INSTANCE") == nullptr;
	bool expected = false;
	m_ownsInProcessSlot = s_inProcessTaken.compare_exchange_strong(expected, true);
	m_useRemote = !m_ownsInProcessSlot && helpersAllowed;
	m_reservedInProcess = !m_useRemote;
	if (m_useRemote) m_remote = std::make_unique<RemoteEngine>();
	return m_reservedInProcess;
}

void EngineFacade::releaseReservation()
{
	if (!m_ownsInProcessSlot) return;
	m_ownsInProcessSlot = false;
	s_inProcessTaken.store(false);
}

bool EngineFacade::start(const std::vector<std::string> &args)
{
	(void) reserveInProcess();
	if (!m_useRemote)
		return m_local.start(args);
	return m_remote->start(args, std::getenv("PROFLIGACY_CI_EXPOSE_LCD_STATE") == nullptr);
}

void EngineFacade::requestStop()
{
	if (m_remote) m_remote->requestStop();
	m_local.requestStop();
}

void EngineFacade::stop()
{
	if (m_remote) m_remote->stop();
	m_local.stop();
}
