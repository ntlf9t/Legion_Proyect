/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/// \addtogroup Trinityd Trinity Daemon
/// @{
/// \file

#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/bind/bind.hpp>
#include <boost/program_options.hpp>
#include <cds/gc/hp.h>
#include <cds/init.h>
#include <google/protobuf/stubs/common.h>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>

#include "IpNetwork.h"
#include "Resolver.h"
#include "DatabaseLoader.h"
#include "AsyncAcceptor.h"
#include "BattlegroundMgr.h"
#include "BigNumber.h"
#include "CliRunnable.h"
#include "Common.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "GitRevision.h"
#include "IoContext.h"
#include "MapInstanced.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "OpenSSLCrypto.h"
#include "OutdoorPvPMgr.h"
#include "ProcessPriority.h"
#include "RASession.h"
#include "RealmList.h"
#include "ScriptLoader.h"
#include "ScriptMgr.h"
#include "segvcatch.h"
#include "TCSoap.h"
#include "ThreadPoolMgr.hpp"
#include "World.h"
#include "WorldSocket.h"
#include "WorldSocketMgr.h"
#include "Banner.h"

#ifdef WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#endif

using namespace boost::program_options;

#ifndef _TRINITY_CORE_CONFIG
    #define _TRINITY_CORE_CONFIG  "worldserver.conf"
#endif

#define WORLD_SLEEP_CONST 10

#ifdef _WIN32
#include "ServiceWin32.h"
char serviceName[] = "worldserver";
char serviceLongName[] = "NordrassilCore world service";
char serviceDescription[] = "NordrassilCore World of Warcraft emulator world service";
/*
 * -1 - not in service mode
 *  0 - stopped
 *  1 - running
 *  2 - paused
 */
int m_ServiceStatus = -1;
#endif

Trinity::Asio::IoContext _ioContext;
boost::asio::deadline_timer _freezeCheckTimer(_ioContext);

std::vector<uint32> _lastMapChangeMsTime;
std::vector<uint32> _mapLoopCounter;

std::vector<uint32> _lastZoneChangeMsTime;
std::vector<uint32> _zoneLoopCounter;

uint32 _worldLoopCounter(0);
uint32 _lastChangeMsTime(0);
uint32 _maxCoreStuckTimeInMs(0);
uint32 _maxMapStuckTimeInMs(0);

void SignalHandler(const boost::system::error_code& error, int signalNumber);
void FreezeDetectorHandler(const boost::system::error_code& error);
AsyncAcceptor* StartRaSocketAcceptor(Trinity::Asio::IoContext& ioContext);
bool StartDB();
void StopDB();
void WorldUpdateLoop();
void ClearOnlineAccounts();
void ShutdownThreadPool(std::vector<std::thread>& threadPool);
bool LoadRealmInfo();
variables_map GetConsoleArguments(int argc, char** argv, std::string& cfg_file, std::string& cfg_service);

/// Launch the Trinity server
extern int main(int argc, char **argv)
{
    // signal(SIGABRT, &Trinity::DumpHandler);

    m_stopEvent = false;
    m_worldCrashChecker = false;
    std::string configFile = _TRINITY_CORE_CONFIG;
    std::string configService;

    auto vm = GetConsoleArguments(argc, argv, configFile, configService);
    // exit if help is enabled
    if (vm.count("help"))
        return 0;

    GOOGLE_PROTOBUF_VERIFY_VERSION;

#ifdef _WIN32
    if (configService.compare("install") == 0)
        return WinServiceInstall() ? 0 : 1;
    if (configService.compare("uninstall") == 0)
        return WinServiceUninstall() ? 0 : 1;
    if (configService.compare("run") == 0)
        WinServiceRun();
#endif

    std::string configError;
    if (!sConfigMgr->LoadInitial(configFile, configError))
    {
        printf("Error in config file: %s\n", configError.c_str());
        return 1;
    }

    Trinity::Banner::Show("worldserver-daemon", [](char const* text)
    {
        TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "%s", text);
    }, []()
    {
        TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "Using configuration file %s.", sConfigMgr->GetFilename().c_str());
        TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "Using SSL version: %s (library: %s)", OPENSSL_VERSION_TEXT, SSLeay_version(SSLEAY_VERSION));
        TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "Using Boost version: %i.%i.%i", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
    }
    );

    OpenSSLCrypto::threadsSetup();
    cds::Initialize();
    cds::gc::HP hpGC;
    cds::threading::Manager::attachThread();

    // Seed the OpenSSL's PRNG here.
    // That way it won't auto-seed when calling BigNumber::SetRand and slow down the first world login
    BigNumber seed;
    seed.SetRand(16 * 8);

    /// worldserver PID file creation
    std::string pidFile = sConfigMgr->GetStringDefault("PidFile", "");
    uint32 pid = 0;
    if (!pidFile.empty())
    {
        if ((pid = CreatePIDFile(pidFile)))
		{
            TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "Daemon PID: %u\n", pid);
		}
        else
        {
            TC_LOG_ERROR(LOG_FILTER_WORLDSERVER, "Cannot create PID file %s.\n", pidFile.c_str());
            return 1;
        }
    }

    // Set signal handlers (this must be done before starting io_service threads, because otherwise they would unblock and exit)
    boost::asio::signal_set signals(_ioContext, SIGINT, SIGTERM);
#if PLATFORM == PLATFORM_WINDOWS
    signals.add(SIGBREAK);
#endif
    signals.async_wait(SignalHandler);

    // Start the Boost based thread pool
    int numThreads = sConfigMgr->GetIntDefault("ThreadPool", 1);
    std::vector<std::thread> threadPool;

    if (numThreads < 1)
        numThreads = 1;

    for (int i = 0; i < numThreads; ++i)
        threadPool.emplace_back(boost::bind(&Trinity::Asio::IoContext::run, &_ioContext));

    // Set process priority according to configuration settings
    SetProcessPriority("server.worldserver");

    // Start the databases
    if (!StartDB())
    {
        ShutdownThreadPool(threadPool);
        return 1;
    }

    // Set server offline (not connectable)
    LoginDatabase.DirectPExecute("UPDATE realmlist SET flag = flag | %u WHERE id = '%d'", REALM_FLAG_OFFLINE, realm.Id.Realm);
   
    sRealmList->Initialize(_ioContext, sConfigMgr->GetIntDefault("RealmsStateUpdateDelay", 10));
    LoadRealmInfo();

    // Initialize the World
    sScriptMgr->SetScriptLoader(AddScripts);
    sWorld->SetInitialWorldSettings();

    _lastMapChangeMsTime.resize(sMapMgr->_mapCount - 1, 0); // If mapID > 2000 change this
    _mapLoopCounter.resize(sMapMgr->_mapCount - 1, 0); // If mapID > 2000 change this
    _lastZoneChangeMsTime.resize(10000, 0); // If zoneID > 10000 change this
    _zoneLoopCounter.resize(10000 - 1, 0); // If zoneID > 10000 change this

    // Launch CliRunnable thread
    std::thread* cliThread = nullptr;
#ifdef _WIN32
    if (sConfigMgr->GetBoolDefault("Console.Enable", true) && (m_ServiceStatus == -1)/* need disable console in service mode*/)
#else
    if (sConfigMgr->GetBoolDefault("Console.Enable", true))
#endif
    {
        cliThread = new std::thread(CliThread);
    }

    // Start the Remote Access port (acceptor) if enabled
    AsyncAcceptor* raAcceptor = nullptr;
    if (sConfigMgr->GetBoolDefault("Ra.Enable", false))
        raAcceptor = StartRaSocketAcceptor(_ioContext);

    // Start soap serving thread if enabled
    std::thread* soapThread = nullptr;
    if (sConfigMgr->GetBoolDefault("SOAP.Enabled", false))
    {
        soapThread = new std::thread(TCSoapThread, sConfigMgr->GetStringDefault("SOAP.IP", "127.0.0.1"), uint16(sConfigMgr->GetIntDefault("SOAP.Port", 7878)));
    }

    uint16 worldPort = uint16(sWorld->getIntConfig(CONFIG_PORT_WORLD));
    std::string worldListener = sConfigMgr->GetStringDefault("BindIP", "0.0.0.0");

    int networkThreads = sConfigMgr->GetIntDefault("Network.Threads", 1);
    if (networkThreads <= 0)
    {
        TC_LOG_ERROR(LOG_FILTER_GENERAL, "Network.Threads must be greater than 0");
        return false;
    }

    sWorldSocketMgr.StartNetwork(_ioContext, worldListener, worldPort, networkThreads);
    // Set server online (allow connecting now)
    LoginDatabase.DirectPExecute("UPDATE realmlist SET flag = flag & ~%u, population = 0 WHERE id = '%u'", REALM_FLAG_OFFLINE, realm.Id.Realm);
    realm.PopulationLevel = 0.0f;
    realm.Flags = RealmFlags(realm.Flags & ~uint32(REALM_FLAG_OFFLINE));

    // Start the freeze check callback cycle in 5 seconds (cycle itself is 1 sec)
    if (int coreStuckTime = sConfigMgr->GetIntDefault("MaxCoreStuckTime", 0))
    {
        _maxCoreStuckTimeInMs = coreStuckTime * 1000;
        _freezeCheckTimer.expires_from_now(boost::posix_time::seconds(5));
        _freezeCheckTimer.async_wait(FreezeDetectorHandler);
        TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "Starting up anti-freeze thread (%u seconds max stuck time)...", coreStuckTime);
        _maxMapStuckTimeInMs = sConfigMgr->GetIntDefault("MaxMapStuckTime", 60) * 1000;
    }

    //if (sConfigMgr->GetBoolDefault("Log.Async.Enable", false))
    {
        // If logs are supposed to be handled async then we need to pass the io_service into the Log singleton
        Log::instance(nullptr/*&_ioContext*/);
    }

    TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "%s (worldserver-daemon) ready...", GitRevision::GetFullVersion());

    sScriptMgr->OnStartup();

    if (sConfigMgr->GetBoolDefault("Segvcatch.Enable", false))
    {
        segvcatch::init_segv();
        segvcatch::init_abrt();
        segvcatch::init_fpe();
    }
    else if (sConfigMgr->GetBoolDefault("DumpHandler.Enable", false))
    {
        #ifndef WIN32
        signal(SIGSEGV, &Trinity::DumpHandler);
        signal(SIGABRT, &Trinity::DumpHandler);
        signal(SIGFPE, &Trinity::DumpHandler);
        #endif
    }

    WorldUpdateLoop();

    // Shutdown starts here
    ShutdownThreadPool(threadPool);

    sScriptMgr->OnShutdown();

    sWorld->KickAll();                                       // save and kick all players
    sWorld->UpdateSessions(1);                             // real players unload required UpdateSessions call

    // unload battleground templates before different singletons destroyed
    sBattlegroundMgr->DeleteAllBattlegrounds();

    sWorldSocketMgr.StopNetwork();

    sMapMgr->UnloadAll();                     // unload all grids (including locked in memory)

    sThreadPoolMgr->stop();

    sObjectAccessor->UnloadAll();             // unload 'i_player2corpse' storage and remove from world
    sScriptMgr->Unload();
    sOutdoorPvPMgr->Die();

    cds::threading::Manager::detachThread();
    cds::Terminate();

    // set server offline
    LoginDatabase.DirectPExecute("UPDATE realmlist SET flag = flag | %u WHERE id = '%d'", REALM_FLAG_OFFLINE, realm.Id.Realm);
    sRealmList->Close();

    // Clean up threads if any
    if (soapThread != nullptr)
    {
        soapThread->join();
        delete soapThread;
    }

    delete raAcceptor;

    ///- Clean database before leaving
    ClearOnlineAccounts();

    StopDB();

    TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "Halting process...");

    if (cliThread != nullptr)
    {
#ifdef _WIN32

        // this only way to terminate CLI thread exist at Win32 (alt. way exist only in Windows Vista API)
        //_exit(1);
        // send keyboard input to safely unblock the CLI thread
        INPUT_RECORD b[4];
        HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
        b[0].EventType = KEY_EVENT;
        b[0].Event.KeyEvent.bKeyDown = TRUE;
        b[0].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[0].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[0].Event.KeyEvent.wRepeatCount = 1;

        b[1].EventType = KEY_EVENT;
        b[1].Event.KeyEvent.bKeyDown = FALSE;
        b[1].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[1].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[1].Event.KeyEvent.wRepeatCount = 1;

        b[2].EventType = KEY_EVENT;
        b[2].Event.KeyEvent.bKeyDown = TRUE;
        b[2].Event.KeyEvent.dwControlKeyState = 0;
        b[2].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[2].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[2].Event.KeyEvent.wRepeatCount = 1;
        b[2].Event.KeyEvent.wVirtualScanCode = 0x1c;

        b[3].EventType = KEY_EVENT;
        b[3].Event.KeyEvent.bKeyDown = FALSE;
        b[3].Event.KeyEvent.dwControlKeyState = 0;
        b[3].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[3].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[3].Event.KeyEvent.wVirtualScanCode = 0x1c;
        b[3].Event.KeyEvent.wRepeatCount = 1;
        DWORD numb;
        WriteConsoleInput(hStdIn, b, 4, &numb);
#endif
        cliThread->join();
        delete cliThread;
    }

    OpenSSLCrypto::threadsCleanup();
    google::protobuf::ShutdownProtobufLibrary();

    // 0 - normal shutdown
    // 1 - shutdown at error
    // 2 - restart command used, this code can be used by restarter for restart Trinityd

    return World::GetExitCode();
}

bool LoadRealmInfo()
{
    boost::asio::ip::tcp::resolver resolver(_ioContext);

    QueryResult result = LoginDatabase.PQuery("SELECT id, name, address, localAddress, localSubnetMask, port, icon, flag, timezone, allowedSecurityLevel, population, gamebuild, Region, Battlegroup FROM realmlist WHERE id = %u", realm.Id.Realm);
    if (!result)
        return false;

    Field* fields = result->Fetch();
    realm.Name = fields[1].GetString();
    Optional<boost::asio::ip::tcp::endpoint> externalAddressQuery = Trinity::Net::Resolve(resolver, tcp::v4(), fields[2].GetString(), "");

    if (!externalAddressQuery)
    {
        TC_LOG_ERROR(LOG_FILTER_WORLDSERVER, "Could not resolve address %s", fields[2].GetString().c_str());
        return false;
    }

    realm.ExternalAddress = std::make_unique<boost::asio::ip::address>(std::move(externalAddressQuery->address()));

	Optional<boost::asio::ip::tcp::endpoint> localAddressQuery = Trinity::Net::Resolve(resolver, tcp::v4(), fields[3].GetString(), "");
	if (!localAddressQuery)
    {
        TC_LOG_ERROR(LOG_FILTER_WORLDSERVER, "Could not resolve address %s", fields[3].GetString().c_str());
        return false;
    }

    realm.LocalAddress = std::make_unique<boost::asio::ip::address>(std::move(localAddressQuery->address()));

	Optional<boost::asio::ip::tcp::endpoint> localSubmaskQuery = Trinity::Net::Resolve(resolver, tcp::v4(), fields[4].GetString(), "");
	if (!localSubmaskQuery)
    {
        TC_LOG_ERROR(LOG_FILTER_WORLDSERVER, "Could not resolve address %s", fields[4].GetString().c_str());
        return false;
    }

    realm.LocalSubnetMask = std::make_unique<boost::asio::ip::address>(std::move(localSubmaskQuery->address()));

    realm.Port = fields[5].GetUInt16();
    realm.Type = fields[6].GetUInt8();
    realm.Flags = RealmFlags(fields[7].GetUInt8());
    realm.Timezone = fields[8].GetUInt8();
    realm.AllowedSecurityLevel = AccountTypes(fields[9].GetUInt8());
    realm.PopulationLevel = fields[10].GetFloat();
    realm.Id.Region = fields[12].GetUInt8();
    realm.Id.Site = fields[13].GetUInt8();
    realm.Build = fields[11].GetUInt32();
    return true;
}

void ShutdownThreadPool(std::vector<std::thread>& threadPool)
{
    _ioContext.stop();

    for (auto& thread : threadPool)
        thread.join();
}

void WorldUpdateLoop()
{
    uint32 realCurrTime = 0;
    uint32 realPrevTime = getMSTime();

    uint32 prevSleepTime = 0;                               // used for balanced full tick time length near WORLD_SLEEP_CONST

    ///- While we have not World::m_stopEvent, update the world
    while (!World::IsStopped())
    {
        ++World::m_worldLoopCounter;
        realCurrTime = getMSTime();

        uint32 diff = getMSTimeDiff(realPrevTime, realCurrTime);

        sWorld->Update(diff);
        realPrevTime = realCurrTime;

        // diff (D0) include time of previous sleep (d0) + tick time (t0)
        // we want that next d1 + t1 == WORLD_SLEEP_CONST
        // we can't know next t1 and then can use (t0 + d1) == WORLD_SLEEP_CONST requirement
        // d1 = WORLD_SLEEP_CONST - t0 = WORLD_SLEEP_CONST - (D0 - d0) = WORLD_SLEEP_CONST + d0 - D0
        if (diff <= WORLD_SLEEP_CONST + prevSleepTime)
        {
            prevSleepTime = WORLD_SLEEP_CONST + prevSleepTime - diff;

            std::this_thread::sleep_for(Milliseconds(prevSleepTime));
        }
        else
            prevSleepTime = 0;

#ifdef _WIN32
        if (m_ServiceStatus == 0)
            World::StopNow(SHUTDOWN_EXIT_CODE);

        while (m_ServiceStatus == 2)
            Sleep(1000);
#endif
    }
}

void SignalHandler(const boost::system::error_code& error, int /*signalNumber*/)
{
    if (!error)
        World::StopNow(SHUTDOWN_EXIT_CODE);
}

void FreezeDetectorHandler(const boost::system::error_code& error)
{
    if (!error && !m_worldCrashChecker)
    {
        uint32 curtime = getMSTime();

        uint32 worldLoopCounter = World::m_worldLoopCounter;
        if (_worldLoopCounter != worldLoopCounter)
        {
            _lastChangeMsTime = curtime;
            _worldLoopCounter = worldLoopCounter;
        }
        // possible freeze
        else if (getMSTimeDiff(_lastChangeMsTime, curtime) > _maxCoreStuckTimeInMs)
        {
            TC_LOG_ERROR(LOG_FILTER_WORLDSERVER, "World Thread hangs, kicking out server!");
            ASSERT(false);
        }

        for (uint32 i = 0; i < _lastMapChangeMsTime.size(); ++i)
        {
            if (Map* map = sMapMgr->FindBaseMap(i))
            {
                if (map->CanCreatedZone())
                {

                    MapInstanced* instances = reinterpret_cast<MapInstanced*>(map);
                    if (!instances)
                        continue;
                    for (InstancedMaps::iterator iter = instances->m_InstancedMaps.begin(); iter != instances->m_InstancedMaps.end(); ++iter)
                    {
                        if (Map* instanced = iter->second)
                        {
                            uint32 lastMsTime = _lastZoneChangeMsTime[instanced->GetInstanceId()];
                            uint32 mapLoopCounter = instanced->m_mapLoopCounter;
                            if (_zoneLoopCounter[instanced->GetInstanceId()] != mapLoopCounter || !mapLoopCounter)
                            {
                                _lastZoneChangeMsTime[instanced->GetInstanceId()] = curtime;
                                _zoneLoopCounter[instanced->GetInstanceId()] = mapLoopCounter;
                            }
                            // possible freeze
                            else if (getMSTimeDiff(lastMsTime, curtime) > _maxMapStuckTimeInMs)
                            {
                                TC_LOG_ERROR(LOG_FILTER_WORLDSERVER, "Map Thread hangs, kicking map %u zone %u diff %u thread!", i, instanced->GetInstanceId(), getMSTimeDiff(lastMsTime, curtime));
                                #ifndef WIN32
                                time_t tt = SystemClock::to_time_t(SystemClock::now());
                                std::tm aTm;
                                localtime_r(&tt, &aTm);
                                sLog->outFreeze("FreezeDetector pid %u mapid %u zone %u", GetPID(), i, instanced->GetInstanceId());

                                // Uncomment this when use exe without gdb
                                /*char buf[20];
                                snprintf(buf, 20, "%04d-%02d-%02d_%02d-%02d-%02d", aTm.tm_year+1900, aTm.tm_mon+1, aTm.tm_mday, aTm.tm_hour, aTm.tm_min, aTm.tm_sec);
                                std::string buffer("gdb --pid " + std::to_string(GetPID()) + " -x freeze.gdb --batch>./errorlog/" + std::string(buf) + "_freeze_mapid_" + std::to_string(i) + ".log 2>./errorlog/" + std::string(buf) + "_freeze_mapid_" + std::to_string(i) + ".err");
                                system(buffer.c_str());*/
                                #endif

                                instanced->TerminateThread();

                                if (std::thread* _thread = instances->_zoneThreads[instanced->GetInstanceId()])
                                {
                                    sLog->outFreeze("Map %u zone %u detach thread", i, instanced->GetInstanceId());
                                    _thread->detach();
                                    sLog->outFreeze("Map %u zone %u cancel thread", i, instanced->GetInstanceId());
                                    #ifndef WIN32
                                    pthread_cancel(_thread->native_handle());
                                    #else
                                    TerminateThread(_thread->native_handle(), 0);
                                    #endif
                                    sLog->outFreeze("Map %u zone %u delete thread", i, instanced->GetInstanceId());
                                    delete _thread;
                                }
                                sLog->outFreeze("Map %u zone %u start new thread", i, instanced->GetInstanceId());
                                instances->_zoneThreads[instanced->GetInstanceId()] = new std::thread(&Map::UpdateLoop, instanced, instanced->GetInstanceId());
                                _lastZoneChangeMsTime[instanced->GetInstanceId()] = curtime;
                            }
                        }
                    }
                }
                else
                {
                    uint32 mapLoopCounter = map->m_mapLoopCounter;
                    if (_mapLoopCounter[i] != mapLoopCounter)
                    {
                        _lastMapChangeMsTime[i] = curtime;
                        _mapLoopCounter[i] = mapLoopCounter;
                    }
                    // possible freeze
                    else if (getMSTimeDiff(_lastMapChangeMsTime[i], curtime) > _maxMapStuckTimeInMs)
                    {
                        TC_LOG_ERROR(LOG_FILTER_WORLDSERVER, "Map Thread hangs, kicking map %u thread!", i);
                        #ifndef WIN32
                        time_t tt = SystemClock::to_time_t(SystemClock::now());
                        std::tm aTm;
                        localtime_r(&tt, &aTm);
                        sLog->outFreeze("FreezeDetector pid %u mapid %u", GetPID(), i);

                        // Uncomment this when use exe without gdb
                        /*char buf[20];
                        snprintf(buf, 20, "%04d-%02d-%02d_%02d-%02d-%02d", aTm.tm_year+1900, aTm.tm_mon+1, aTm.tm_mday, aTm.tm_hour, aTm.tm_min, aTm.tm_sec);
                        std::string buffer("gdb --pid " + std::to_string(GetPID()) + " -x freeze.gdb --batch>./errorlog/" + std::string(buf) + "_freeze_mapid_" + std::to_string(i) + ".log 2>./errorlog/" + std::string(buf) + "_freeze_mapid_" + std::to_string(i) + ".err");
                        system(buffer.c_str());*/
                        #endif

                        map->TerminateThread();

                        if (std::thread* _thread = sMapMgr->_mapThreads[i])
                        {
                            sLog->outFreeze("Map %u detach thread", i);
                            _thread->detach();
                            sLog->outFreeze("Map %u cancel thread", i);
                            #ifndef WIN32
                            pthread_cancel(_thread->native_handle());
                            #else
                            TerminateThread(_thread->native_handle(), 0);
                            #endif
                            sLog->outFreeze("Map %u delete thread", i);
                            delete _thread;
                        }
                        sLog->outFreeze("Map %u start new thread", i);
                        sMapMgr->_mapThreads[i] = new std::thread(&Map::UpdateLoop, map, i);
                        _lastMapChangeMsTime[i] = curtime;
                    }
                }
            }
            else
            {
                _lastMapChangeMsTime[i] = curtime;
                _mapLoopCounter[i] = 0;
            }
        }

        _freezeCheckTimer.expires_from_now(boost::posix_time::seconds(1));
        _freezeCheckTimer.async_wait(FreezeDetectorHandler);
    }
}

AsyncAcceptor* StartRaSocketAcceptor(Trinity::Asio::IoContext& ioContext)
{
    uint16 raPort = uint16(sConfigMgr->GetIntDefault("Ra.Port", 3443));
    std::string raListener = sConfigMgr->GetStringDefault("Ra.IP", "0.0.0.0");

    AsyncAcceptor* acceptor = new AsyncAcceptor(ioContext, raListener, raPort);
    acceptor->AsyncAccept<RASession>();
    return acceptor;
}

/// Initialize connection to the databases
bool StartDB()
{
    MySQL::Library_Init();

	// Load databases
	DatabaseLoader loader("server.worldserver", DatabaseLoader::DATABASE_NONE);
	loader
		.AddDatabase(LoginDatabase, "Login")
		.AddDatabase(CharacterDatabase, "Character")
		.AddDatabase(WorldDatabase, "World")
		.AddDatabase(HotfixDatabase, "Hotfix");

	if (!loader.Load())
		return false;

    ///- Get the realm Id from the configuration file
    realm.Id.Realm = sConfigMgr->GetIntDefault("RealmID", 0);
    if (!realm.Id.Realm)
    {
        TC_LOG_ERROR(LOG_FILTER_WORLDSERVER, "Realm ID not defined in configuration file");
        return false;
    }

    TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "Realm running as realm ID %u", realm.Id.Realm);

    ///- Clean the database before starting
    ClearOnlineAccounts();

    ///- Insert version info into DB
    //WorldDatabase.PExecute("UPDATE version SET core_version = '%s', core_revision = '%s'", GitRevision::GetFullVersion(), _HASH);        // One-time query

    sWorld->LoadDBVersion();

    TC_LOG_INFO(LOG_FILTER_WORLDSERVER, "Using World DB: %s", sWorld->GetDBVersion());
    return true;
}

void StopDB()
{
    CharacterDatabase.Close();
    WorldDatabase.Close();
    LoginDatabase.Close();

    MySQL::Library_End();
}

/// Clear 'online' status for all accounts with characters in this realm
void ClearOnlineAccounts()
{
    // Reset online status for all accounts with characters on the current realm
    LoginDatabase.DirectPExecute("UPDATE account SET online = 0 WHERE online > 0 AND id IN (SELECT acctid FROM realmcharacters WHERE realmid = %d)", realm.Id.Realm);

    // Reset online status for all characters
    CharacterDatabase.DirectExecute("UPDATE characters SET online = 0 WHERE online <> 0");

    // Battleground instance ids reset at server restart
    CharacterDatabase.DirectExecute("UPDATE character_battleground_data SET instanceId = 0");
}

/// @}

variables_map GetConsoleArguments(int argc, char** argv, std::string& configFile, std::string& configService)
{
    options_description all("Allowed options");
    all.add_options()
        ("help,h", "print usage message")
        ("config,c", value<std::string>(&configFile)->default_value(_TRINITY_CORE_CONFIG), "use <arg> as configuration file")
        ;
#ifdef _WIN32
    options_description win("Windows platform specific options");
    win.add_options()
        ("service,s", value<std::string>(&configService)->default_value(""), "Windows service options: [install | uninstall]")
        ;

    all.add(win);
#endif
    variables_map vm;
    try
    {
        store(command_line_parser(argc, argv).options(all).allow_unregistered().run(), vm);
        notify(vm);
    }
    catch (std::exception& e) {
        std::cerr << e.what() << "\n";
    }

    if (vm.count("help")) {
        std::cout << all << "\n";
    }

    return vm;
}
