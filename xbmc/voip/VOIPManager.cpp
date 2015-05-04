/*
 *      Copyright (C) 2012 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "Application.h"
#include "ApplicationMessenger.h"
#include "GUIInfoManager.h"
#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogNumeric.h"
#include "dialogs/GUIDialogProgress.h"
#include "dialogs/GUIDialogExtendedProgressBar.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "music/tags/MusicInfoTag.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/Stopwatch.h"
#include "utils/StringUtils.h"
#include "threads/Atomics.h"
#include "utils/JobManager.h"

#include "VOIPManager.h"
#include "addons/VOIPClient.h"

#include "interfaces/AnnouncementManager.h"
#include "addons/AddonInstaller.h"

using namespace std;
using namespace VOIP;

CVOIPManager::CVOIPManager(void) :
		CThread("VOIP manager"),
		m_triggerEvent(true),
		m_managerState(ManagerStateStopped)
{
	CLog::Log(LOGDEBUG, "VOIPManager - created");
	ResetProperties();
}

CVOIPManager::~CVOIPManager(void)
{
	Stop();
	CLog::Log(LOGDEBUG, "VOIPManager - destroyed");
}

CVOIPManager &CVOIPManager::Get(void)
{
	static CVOIPManager voipManagerInstance;
	return voipManagerInstance;
}

bool CVOIPManager::InstallAddonAllowed(const std::string& strAddonId) const
{
	return !IsStarted();
}

void CVOIPManager::MarkAsOutdated(const std::string& strAddonId,
		const std::string& strReferer)
{
	if (IsStarted()/* && g_settings.m_bAddonAutoUpdate*/)
	{
		CSingleLock lock(m_critSection);
		m_outdatedAddons.insert(std::make_pair(strAddonId, strReferer));
	}
}

bool CVOIPManager::UpgradeOutdatedAddons(void)
{
	CSingleLock lock(m_critSection);
	if (m_outdatedAddons.empty())
		return true;

	// there's add-ons that couldn't be updated
	for (map<string, string>::iterator it = m_outdatedAddons.begin();
			it != m_outdatedAddons.end(); it++)
	{
		if (!InstallAddonAllowed(it->first))
		{
			// we can't upgrade right now
			return true;
		}
	}

	// all outdated add-ons can be upgraded now
	CLog::Log(LOGINFO, "VOIPManager - upgrading outdated add-ons");

	map<string, string> outdatedAddons = m_outdatedAddons;
	// stop threads and unload
	SetState(ManagerStateInterrupted);
	m_clientsManager->Stop();
	Cleanup();

	// upgrade all add-ons
	for (map<string, string>::iterator it = outdatedAddons.begin();
			it != outdatedAddons.end(); it++)
	{
		CLog::Log(LOGINFO, "VOIPManager - updating add-on '%s'",
				it->first.c_str());
		CAddonInstaller::Get().Install(it->first, true, it->second, false);
	}

	// reload
	CLog::Log(LOGINFO, "VOIPManager - %s - restarting the VOIP manager",
			__FUNCTION__);
	SetState(ManagerStateStarting);
	ResetProperties();

	while (!Load() && GetState() == ManagerStateStarting)
	{
		CLog::Log(LOGERROR,
				"VOIPManager - %s - failed to load VOIP data, retrying",
				__FUNCTION__);
		Cleanup();
		Sleep(1000);
	}

	if (GetState() == ManagerStateStarting)
	{
		SetState(ManagerStateStarted);
		m_clientsManager->Start();

		CLog::Log(LOGDEBUG, "VOIPManager - %s - restarted", __FUNCTION__);
		return true;
	}

	return false;
}

void CVOIPManager::Cleanup(void)
{
	CSingleLock lock(m_critSection);

	if (m_clientsManager)
	{
		m_clientsManager->Stop();
		SAFE_DELETE(m_clientsManager);
	}
	m_triggerEvent.Set();

	for (unsigned int iJobPtr = 0; iJobPtr < m_pendingUpdates.size(); iJobPtr++)
		delete m_pendingUpdates.at(iJobPtr);
	m_pendingUpdates.clear();



	m_initialisedEvent.Reset();
}

void CVOIPManager::ResetProperties(void)
{
	CSingleLock lock(m_critSection);
	Cleanup();

	if (!g_application.m_bStop)
	{
		m_clientsManager = new CVOIPClientsManager;
	}
}

void CVOIPManager::Start(bool bAsync /* = false */,
		bool bOpenVOIPWindow /* = false */)
{
	CLog::Log(LOGNOTICE, "VOIPManager - %s ", __FUNCTION__);
	CSingleLock lock(m_critSection);


		Stop();
		if (!CSettings::Get().GetBool("voipmanager.enabled") && false)
		{
		    CLog::Log(LOGNOTICE, "VOIPManager - %s : not starting, voipmanager is not enabled", __FUNCTION__);
		    return;
		}

		ResetProperties();



	SetState(ManagerStateStarting);

	if(m_clientsManager){
		m_clientsManager->Start();
	}
}

void CVOIPManager::Stop(void)
{
	CLog::Log(LOGNOTICE, "VOIPManager - %s ", __FUNCTION__);

	if (GetState() == ManagerStateStopping || GetState() == ManagerStateStopped)
		return;

	SetState(ManagerStateStopping);

	/* unload all data */
	Cleanup();
}

ManagerState CVOIPManager::GetState(void) const
{
	CSingleLock lock(m_managerStateMutex);
	return m_managerState;
}

void CVOIPManager::SetState(ManagerState state)
{
	CSingleLock lock(m_managerStateMutex);
	m_managerState = state;
}

void CVOIPManager::Process(void)
{
	CLog::Log(LOGNOTICE, "VOIPManager - %s ", __FUNCTION__);

	while (!Load() && GetState() == ManagerStateStarting)
	{
		CLog::Log(LOGERROR,
				"VOIPManager - %s - failed to load VOIP data, retrying",
				__FUNCTION__);
		if(m_clientsManager) m_clientsManager->Stop();
		Cleanup();
		Sleep(1000);
	}

	if (GetState() == ManagerStateStarting)
		SetState(ManagerStateStarted);
	else
		return;

	/* main loop */
	CLog::Log(LOGDEBUG, "VOIPManager - %s - entering main loop", __FUNCTION__);
	m_initialisedEvent.Set();

	bool bTerminate
	{ false };
	while (IsStarted() && !bTerminate)
	{
		/* first startup */
		if (m_bFirstStart)
		{

			m_bFirstStart = false;

		}
		/* execute the next pending jobs if there are any */
		try
		{
			//ExecutePendingJobs();
			CLog::Log(LOGDEBUG, "VOIPManager - %s - doing work", __FUNCTION__);
			Sleep(10000);

			//if no jobs, terminate main loop
			if (true)
				bTerminate = true;
		} catch (...)
		{
			CLog::Log(LOGERROR,
					"PVRManager - %s - an error occured while trying to execute the last update job, trying to recover",
					__FUNCTION__);
			bTerminate = true;
		}

		if (!UpgradeOutdatedAddons())
		{
			// failed to load after upgrading
			CLog::Log(LOGERROR,
					"PVRManager - %s - could not load pvr data after upgrading. stopping the pvrmanager",
					__FUNCTION__);
			bTerminate = true;
		}

	}

	if (IsStarted())
	{
		CLog::Log(LOGNOTICE, "VOIPManager - %s - no add-ons enabled anymore",
				__FUNCTION__);
		CApplicationMessenger::Get().SetPVRManagerState(true);
	}
	else
	{
		CLog::Log(LOGNOTICE, "VOIPManager - %s - no add-ons enabled anymore",
				__FUNCTION__);
		CApplicationMessenger::Get().SetPVRManagerState(false);
	}

}

bool CVOIPManager::Load(void)
{
	CLog::Log(LOGDEBUG,
			"VOIPManager - %s - active clients found. continue to start",
			__FUNCTION__);

    if(m_clientsManager) m_clientsManager->Start();

	return true;
}

bool CVOIPManager::IsStarted(void) const
{
	return true;
}
