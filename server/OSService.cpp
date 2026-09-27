/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "OSService.h"
#include <csignal>

using namespace std;
using namespace sptk;
using namespace xmq;

IServerController* OSService::m_controlledServer;
std::atomic_bool   OSService::s_terminationRequested {false};
std::atomic_int    OSService::s_terminationSignal {0};

// Stored from a signal handler, which is only safe for an atomic that never takes a lock.
static_assert(std::atomic_int::is_always_lock_free);

void OSService::requestTermination(const int signal)
{
    // The signal first: the service loop reads it once it has seen the flag.
    s_terminationSignal.store(signal, std::memory_order_relaxed);
    s_terminationRequested.store(true, std::memory_order_release);
}

bool OSService::terminationRequested()
{
    return s_terminationRequested.load(std::memory_order_acquire);
}

int OSService::terminationSignal()
{
    return s_terminationSignal.load(std::memory_order_relaxed);
}

namespace {

/// The signals main() installs a handler for, by name; anything else by number.
String signalName(const int signal)
{
    switch (signal)
    {
        case SIGINT:
            return "SIGINT";
        case SIGTERM:
            return "SIGTERM";
#ifndef _WIN32
        case SIGHUP:
            return "SIGHUP";
#endif
        default:
            return "signal " + to_string(signal);
    }
}

} // namespace

#ifdef _WIN32
namespace {

SERVICE_STATUS        g_ServiceStatus = {};
SERVICE_STATUS_HANDLE g_StatusHandle = nullptr;
HANDLE                g_ServiceStopEvent = INVALID_HANDLE_VALUE;

string serviceError;
} // namespace

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv);

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode)
{
    if (CtrlCode == SERVICE_CONTROL_STOP && g_ServiceStatus.dwCurrentState == SERVICE_RUNNING)
    {
        Logger logger(*OSService::m_controlledServer->getLogEngine());
        // Terminating service here, and waiting for termination to complete
        logger.info("Received service stop command");
        OSService::m_controlledServer->stopService();

        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 4;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        // This will signal the worker thread to start shutting down
        SetEvent(g_ServiceStopEvent);
    }
}

VOID WINAPI ServiceMain(DWORD /*argc*/, LPTSTR* /*argv*/)
{
    serviceError = "";

    auto serviceName = OSService::m_controlledServer->getServiceName();
    g_StatusHandle = RegisterServiceCtrlHandler(serviceName.c_str(), ServiceCtrlHandler);
    if (g_StatusHandle == nullptr)
    {
        serviceError = "ServiceMain: RegisterServiceCtrlHandler returned error";
        return;
    }

    // Tell the service controller we are starting
    ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // Perform tasks necessary to start the service here
    OSService::m_controlledServer->startService();

    // Create stop event to wait on later.
    g_ServiceStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (g_ServiceStopEvent == nullptr)
    {
        serviceError = "ServiceMain: CreateEvent(g_ServiceStopEvent) returned error";
        g_ServiceStatus.dwCheckPoint = 0;
    }
    else
    {
        // Tell the service controller we are started
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
        g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        //  Periodically check if the service has been requested to stop
        while (WaitForSingleObject(g_ServiceStopEvent, 1000) != WAIT_OBJECT_0)
        {
        }

        // Perform any cleanup tasks
        CloseHandle(g_ServiceStopEvent);

        g_ServiceStatus.dwCheckPoint = 3;
    }

    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwWin32ExitCode = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}
#endif

OSService::OSService(LogEngine& logEngine, const String& logPrefix, const bool daemon)
    : m_logger(logEngine, logPrefix)
    , m_runAsService(daemon)
{
}

void OSService::setServerController(IServerController* controlledServer)
{
    m_controlledServer = controlledServer;
}

void OSService::runService()
{
    auto serviceName = m_controlledServer->getServiceName();

    if (m_controlledServer == nullptr)
    {
        m_logger.error(serviceName + " : server not set.");
        return;
    }

    m_logger.notice(serviceName + " starting");

#ifdef _WIN32
    SERVICE_TABLE_ENTRY ServiceTable[] =
        {
            {bit_cast<LPSTR>(serviceName.c_str()), static_cast<LPSERVICE_MAIN_FUNCTIONA>(ServiceMain)},
            {nullptr, nullptr}};

    if (StartServiceCtrlDispatcher(ServiceTable) == FALSE)
    {
        // Get Windows last error
        LPCTSTR lpMsgBuf = nullptr;
        DWORD   dw = GetLastError();
        FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            dw,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPTSTR>(&lpMsgBuf),
            0, nullptr);
        if (lpMsgBuf)
            m_logger.error(string(serviceName) + " returned error: " + string(lpMsgBuf));
    }
#endif
    // Nothing for anything but Windows: elsewhere the broker always runs in the foreground and
    // execute() never comes here. See the note in main.cpp about the fork this used to do.
}

void OSService::execute()
{
    m_logger.info("Starting");
    if (m_runAsService)
    {

        runService();
    }
    else
    {
        m_controlledServer->startService();
        while (!terminationRequested() && !m_controlledServer->isStopped(chrono::seconds(1)))
        {
            // Waiting until the server is stopped or termination is requested
        }
        // Said before the shutdown, so it is the line above "Server stopped." A clean stop with no
        // reason beside it reads as the broker deciding to quit: on 2026-09-28 three restarts by
        // needrestart during an AWS campaign looked exactly like that until systemd's journal was read.
        if (const auto signal = terminationSignal(); signal != 0)
        {
            m_logger.info("Received " + signalName(signal) + " (" + to_string(signal) + "), stopping the server.");
        }
        m_controlledServer->stopService();
    }
}

void OSService::terminate()
{
#ifdef _WIN32
    if (m_runAsService)
    {
        ServiceCtrlHandler(SERVICE_CONTROL_STOP);
        return;
    }
#else
    // Nothing to on Linux
#endif
}

void OSService::installService()
{
#ifdef _WIN32
    TCHAR szUnquotedPath[MAX_PATH];

    if (!GetModuleFileName(nullptr, szUnquotedPath, MAX_PATH))
    {
        throw SystemException("Cannot get module name");
    }

    // In case the path contains a space, it must be quoted so that
    // it is correctly interpreted. For example,
    // "d:\my share\myService.exe" should be specified as
    // ""d:\my share\myService.exe"".
    TCHAR szPath[MAX_PATH];
    snprintf(szPath, MAX_PATH, "\"%s\"", szUnquotedPath);

    SC_HANDLE schSCManager {nullptr};
    SC_HANDLE schService {nullptr};

    try
    {
        if (m_controlledServer == nullptr)
        {
            throw Exception("Controlled server is not set.");
        }

        // Get a handle to the SCM database.
        schSCManager = OpenSCManager(
            nullptr,                // local computer
            nullptr,                // ServicesActive database
            SC_MANAGER_ALL_ACCESS); // full access rights

        if (nullptr == schSCManager)
        {
            throw SystemException("OpenSCManager failed");
        }

        // Create the service
        schService = CreateService(
            schSCManager,                                        // SCM database
            m_controlledServer->getServiceName().c_str(),        // name of service
            m_controlledServer->getServiceDescription().c_str(), // service name to display
            SERVICE_ALL_ACCESS,                                  // desired access
            SERVICE_WIN32_OWN_PROCESS,                           // service type
            SERVICE_DEMAND_START,                                // start type
            SERVICE_ERROR_NORMAL,                                // error control type
            szPath,                                              // path to service's binary
            nullptr,                                             // no load ordering group
            nullptr,                                             // no tag identifier
            nullptr,                                             // no dependencies
            nullptr,                                             // LocalSystem account
            nullptr);                                            // no password

        if (schService == nullptr)
        {
            throw SystemException("CreateService failed");
        }
        puts("* Service installed successfully");
    }
    catch (const Exception& e)
    {
        m_logger.error(e.what());
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
#endif
}

void OSService::uninstallService()
{
#ifdef _WIN32
    SC_HANDLE schSCManager {nullptr};
    SC_HANDLE schService {nullptr};

    try
    {
        if (m_controlledServer == nullptr)
        {
            throw Exception("Controlled server is not set.");
        }

        // Get a handle to the SCM database.
        schSCManager = OpenSCManager(
            nullptr,                // local computer
            nullptr,                // ServicesActive database
            SC_MANAGER_ALL_ACCESS); // full access rights

        if (nullptr == schSCManager)
        {
            throw SystemException("OpenSCManager failed");
        }

        // Open the service
        schService = OpenService(
            schSCManager,                                 // SCM database
            m_controlledServer->getServiceName().c_str(), // name of service
            SERVICE_ALL_ACCESS);                          // desired access
        if (schService == nullptr)
        {
            throw SystemException("OpenService failed");
        }

        if (DeleteService(schService))
        {
            puts("* Service uninstalled successfully");
        }
        else
        {
            throw SystemException("DeleteService failed");
        }
    }
    catch (const Exception& e)
    {
        m_logger.error(e.what());
    }

    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
#endif
}
