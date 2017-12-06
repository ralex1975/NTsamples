#include "ServicesMonitor.h"
#include <iostream>
#include <vector>

// ============================================

ThrowSystemError::ThrowSystemError(DWORD errorCode, const char* errorMessage)
{
    throw std::system_error(errorCode, std::system_category(), errorMessage);
}

// ============================================

BaseMonitorDispatcher::BaseMonitorDispatcher() :
    m_dispatcherStartStopEvent(NULL),
    m_dispatcherLockEvent(NULL)
{
    SetMonitorState(MonitorStopped);

    memset(m_controlEvents, 0, sizeof(m_controlEvents));

    try
    {
        // Allocate syncronization events

        m_dispatcherStartStopEvent = AllocateEvent();
        m_dispatcherLockEvent = AllocateEvent();
        m_dispatcherAcceptPauseEvent = AllocateEvent();

        for (auto i = 0; i < _countof(m_controlEvents); i++)
            m_controlEvents[i] = AllocateEvent();

        // Create dispatcher thread

        m_dispatcherThread.reset(new std::thread(NotificationDispatcherRoutine, this));
        m_dispatcherThread->detach();

        auto result = ::WaitForSingleObject(m_dispatcherStartStopEvent, 10000);
        if (result != WAIT_OBJECT_0)
            ThrowSystemError error(result, "BaseMonitorDispatcher::BaseMonitorDispatcher -> WaitForSingleObject()");
    }
    catch (...)
    {
        ReleaseResources();
        std::rethrow_exception(
            std::current_exception()
        );
    }
}

BaseMonitorDispatcher::~BaseMonitorDispatcher()
{
    SetMonitorState(MonitorTerminating);
    
    NotifyDispatcher(StopNotification);
    UnlockDispatcher();

    ::WaitForSingleObject(m_dispatcherStartStopEvent, 10000);

    ReleaseResources();
}

void BaseMonitorDispatcher::ReleaseResources()
{
    m_dispatcherThread.reset();

    if (m_dispatcherStartStopEvent)
        ::CloseHandle(m_dispatcherStartStopEvent);

    if (m_dispatcherLockEvent)
        ::CloseHandle(m_dispatcherLockEvent);

    if (m_dispatcherAcceptPauseEvent)
        ::CloseHandle(m_dispatcherAcceptPauseEvent);

    for (auto i = 0; i < _countof(m_controlEvents); i++)
        if (m_controlEvents[i])
            ::CloseHandle(m_controlEvents[i]);
}

void BaseMonitorDispatcher::StartMonitor()
{
    std::lock_guard<std::mutex> locker(m_controlMutex);

    if (GetMonitorState() != MonitorStopped)
        throw std::exception("BaseMonitorDispatcher::StartMonitor: monitor isn't stopped");

    SetMonitorState(MonitorStarted);
    UnlockDispatcher();
}

void BaseMonitorDispatcher::StopMonitor()
{
    std::lock_guard<std::mutex> locker(m_controlMutex);

    if (GetMonitorState() != MonitorStarted)
        throw std::exception("BaseMonitorDispatcher::StopMonitor: monitor isn't started");

    NotifyDispatcher(StopNotification);

    auto result = ::WaitForSingleObject(m_dispatcherStartStopEvent, 10000);
    if (result != WAIT_OBJECT_0)
        ThrowSystemError error(result, "BaseMonitorDispatcher::StopMonitor -> WaitForSingleObject()");

    SetMonitorState(MonitorStopped);
}

DWORD CALLBACK BaseMonitorDispatcher::NotificationDispatcherRoutine(PVOID parameter)
{
    auto monitor = reinterpret_cast<BaseMonitorDispatcher*>(parameter);

    // Notify that we ready to work
    ::SetEvent(monitor->m_dispatcherStartStopEvent);

    while (true)
    {
        // Wait for dispatcher's unlock
        ::WaitForSingleObject(monitor->m_dispatcherLockEvent, INFINITE);

        monitor->NotificationDispatcher();

        auto state = monitor->GetMonitorState();

        // Notify that we stopped the work
        ::SetEvent(monitor->m_dispatcherStartStopEvent);

        if (state == MonitorTerminating)
            break;
    }

    return 0;
}

void BaseMonitorDispatcher::NotificationDispatcher()
{
    try
    {
        while (true)
        {
            auto status = WaitForMultipleObjectsEx(_countof(m_controlEvents), m_controlEvents, FALSE, INFINITE, TRUE);
            if (status == WAIT_OBJECT_0 + StopNotification)
            {
                break;
            }
            else if (status == WAIT_OBJECT_0 + CallbackNotification)
            {
                std::lock_guard<std::mutex> locker(m_callbacksMutex);
                while (!m_dispatchCallbacks.empty())
                {
                    auto& item = m_dispatchCallbacks.front();
                    (item.first)(item.second);
                    m_dispatchCallbacks.pop();
                }
            }
            else if (status == WAIT_OBJECT_0 + PauseProcessingNotification)
            {
                SetEvent(m_dispatcherAcceptPauseEvent);
                std::lock_guard<std::mutex> locker(m_dispatcherPauseMutex);
            }
            else if (status == WAIT_IO_COMPLETION)
            {
                continue;
            }
            else
            {
                ThrowSystemError error(status, "BaseMonitorDispatcher::NotificationDispatcher -> WaitForMultipleObjectsEx()");
            }
        }
    }
    catch (std::system_error& e)
    {
        std::cout << "BaseMonitorDispatcher::NotificationDispatcher: unhandled system exception: " << e.what() << " (code: " << e.code() << ")" << std::endl;
    }
    catch (std::exception& e)
    {
        std::cout << "BaseMonitorDispatcher::NotificationDispatcher: unhandled exception: " << e.what() << std::endl;
    }
}

void BaseMonitorDispatcher::SetMonitorState(MonitorStates state)
{
    m_monitorState = state;
}

BaseMonitorDispatcher::MonitorStates BaseMonitorDispatcher::GetMonitorState()
{
    return m_monitorState;
}

void BaseMonitorDispatcher::NotifyDispatcher(MonitorNotifications notification)
{
    ::SetEvent(m_controlEvents[notification]);
}

void BaseMonitorDispatcher::UnlockDispatcher()
{
    ::SetEvent(m_dispatcherLockEvent);
}

void BaseMonitorDispatcher::PauseDispatcher()
{
    m_dispatcherPauseMutex.lock();

    NotifyDispatcher(PauseProcessingNotification);

    auto result = ::WaitForSingleObject(m_dispatcherAcceptPauseEvent, INFINITE);
    if (result != WAIT_OBJECT_0)
        ThrowSystemError error(result, "BaseMonitorDispatcher::StopMonitor -> WaitForSingleObject()");
}

void BaseMonitorDispatcher::ResumeDispatcher()
{
    m_dispatcherPauseMutex.unlock();
}

void BaseMonitorDispatcher::PushCallback(MonitorWorkCallback callback, void* parameter)
{
    std::lock_guard<std::mutex> locker(m_callbacksMutex);
    m_dispatchCallbacks.push(
        std::make_pair(callback, parameter)
    );
    NotifyDispatcher(CallbackNotification);
}

const wchar_t* BaseMonitorDispatcher::CleanServiceName(const wchar_t* serviceName)
{
    while (*serviceName)
    {
        if (*serviceName == L'\0')
            throw std::length_error("BaseMonitorDispatcher::CleanServiceName: invalid service name");

        if (*serviceName != L'\\' && *serviceName != L'/')
            break;

        serviceName++;
    }
    return serviceName;
}

HANDLE BaseMonitorDispatcher::AllocateEvent(bool manual, bool initial)
{
    HANDLE event = ::CreateEvent(NULL, (manual ? TRUE : FALSE), (initial ? TRUE : FALSE), NULL);

    if (!event)
        ThrowSystemError error(::GetLastError(), "BaseMonitorDispatcher::AllocateEvent -> CreateEvent()");

    return event;
}

// ============================================

const DWORD ServicesMonitor::s_serviceNotifyMask = SERVICE_NOTIFY_RUNNING | SERVICE_NOTIFY_STOPPED | SERVICE_NOTIFY_CONTINUE_PENDING 
                                                 | SERVICE_NOTIFY_DELETE_PENDING | SERVICE_NOTIFY_PAUSE_PENDING | SERVICE_NOTIFY_PAUSED
                                                 | SERVICE_NOTIFY_START_PENDING | SERVICE_NOTIFY_STOP_PENDING;

ServicesMonitor::ServicesMonitor() :
    m_monitoringStarted(false),
    m_scManager(NULL)
{
    m_scManager = ::OpenSCManager(NULL, SERVICES_ACTIVE_DATABASE, SC_MANAGER_ENUMERATE_SERVICE);
    if (!m_scManager)
        ThrowSystemError error(::GetLastError(), "ServicesMonitor::ServicesMonitor -> OpenSCManager()");
}

ServicesMonitor::~ServicesMonitor()
{
    BaseMonitorDispatcher::StopMonitor();

    for (auto it = m_services.begin(); it != m_services.end(); it++)
        if (it->second.handle)
            ::CloseServiceHandle(it->second.handle);

    ::CloseServiceHandle(m_scManager);
}

void ServicesMonitor::InsertService(const wchar_t* serviceName, const SERVICE_STATUS_PROCESS& serviceStatus)
{
    auto cleanSvcName = CleanServiceName(serviceName);

    ServiceContext serviceContext;
    serviceContext.registred = false;
    serviceContext.name = cleanSvcName;
    serviceContext.status = serviceStatus;
    memset(&serviceContext.context, 0, sizeof(NotificationContext));
    serviceContext.handle = ::OpenServiceW(m_scManager, cleanSvcName, SERVICE_QUERY_STATUS);
    if (!serviceContext.handle)
        ThrowSystemError error(::GetLastError(), "ServicesMonitor::InsertService -> OpenService()");

    std::lock_guard<std::mutex> locker(m_servicesMutex);

    auto result = m_services.insert(std::make_pair(serviceName, serviceContext));
    if (!result.second)
    {
        // The service already in map therefore just update the status
        auto it = *result.first;
        it.second.status = serviceContext.status;
    }
    else
    {
        if (m_monitoringStarted)
            PushCallback(InstallServicesNotification, this);
    }
}

void ServicesMonitor::RemoveService(const wchar_t* serviceName)
{
    auto cleanSvcName = CleanServiceName(serviceName);

    std::lock_guard<std::mutex> locker(m_servicesMutex);

    auto svc = m_services.find(cleanSvcName);
    if (svc == m_services.end())
        throw std::out_of_range("ServicesMonitor::RemoveService: service not found");

    if (svc->second.handle)
        ::CloseServiceHandle(svc->second.handle);

    m_services.erase(svc);
}

void ServicesMonitor::Subscribe(ServiceNotificationCallback callback, void* parameter)
{
    std::lock_guard<std::mutex> locker(m_controlMutex);

    if (m_monitoringStarted)
        throw std::exception("ServicesMonitor::Subscribe: can't subscribe after starting monitoring");

    m_subscibers.insert(m_subscibers.begin(), std::make_pair(callback, parameter));
}

void ServicesMonitor::Unsubscribe(ServiceNotificationCallback callback)
{
    std::lock_guard<std::mutex> locker(m_controlMutex);

    if (m_monitoringStarted)
        throw std::exception("ServicesMonitor::Unsubscribe: can't unsubscribe after starting monitoring");

    for (auto it = m_subscibers.begin(); it != m_subscibers.end(); it++)
    {
        if (it->first == callback)
        {
            m_subscibers.erase(it);
            break;
        }
    }
}

void ServicesMonitor::EnumAndInsertServices()
{
    std::vector<BYTE> buffer;

    buffer.insert(buffer.begin(), sizeof(ENUM_SERVICE_STATUS_PROCESS) * 100, 0);

    DWORD services;
    do
    {
        DWORD needed;

        if (::EnumServicesStatusExW(
                m_scManager,
                SC_ENUM_PROCESS_INFO,
                SERVICE_WIN32 | SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS, 
                SERVICE_STATE_ALL, 
                &buffer[0], 
                (DWORD)buffer.size(),
                &needed,
                &services,
                NULL,
                NULL))
            break;

        auto errorCode = ::GetLastError();
        if (errorCode != ERROR_MORE_DATA)
            ThrowSystemError error(errorCode, "ServicesMonitor::EnumAndInsertServices -> OpenService()");

        buffer.insert(buffer.end(), needed + 100, 0);
    } 
    while (true);

    auto service = (ENUM_SERVICE_STATUS_PROCESS*)&buffer[0];
    for (DWORD i = 0; i < services; i++)
    {
        try
        {
            InsertService(service[i].lpServiceName, service[i].ServiceStatusProcess);
        }
        catch (std::exception& e)
        {
            std::wcout << L"ServicesMonitor::EnumAndInsertServices: unhandled exception for service '" << service[i].lpServiceName << "' : ";
            std::cout << e.what() << std::endl;
        }
    }
}

void ServicesMonitor::StartMonitoring()
{
    std::lock_guard<std::mutex> locker(m_controlMutex);

    if (m_monitoringStarted)
        throw std::exception("ServicesMonitor::StartMonitoring: monitoring is already started");

    m_notification.active = false;
    PushCallback(InstallSCMNotification, this);

    BaseMonitorDispatcher::StartMonitor();

    EnumAndInsertServices();
    PushCallback(InstallServicesNotification, this);

    m_monitoringStarted = true;
}

void ServicesMonitor::StopMonitoring()
{
    std::lock_guard<std::mutex> locker(m_controlMutex);

    if (!m_monitoringStarted)
        throw std::exception("ServicesMonitor::StopMonitoring: monitoring is already stopped");

    {
        std::lock_guard<std::mutex> locker(m_servicesMutex);
        for (auto it = m_services.begin(); it != m_services.end(); it++)
            if (it->second.handle)
            {
                ::CloseServiceHandle(it->second.handle);
                it->second.handle = NULL;
            }
    }

    BaseMonitorDispatcher::StopMonitor();

    m_services.clear();

    m_monitoringStarted = false;
}

void CALLBACK ServicesMonitor::ServiceNotificationDispatcherRoutine(PVOID parameter)
{
    auto& context = *(NotificationContext*)parameter;
    auto& notification = context.notification;
    auto& serviceContext = *(ServiceContext*)context.serviceContext;
    auto& monitor = *context.monitor;

    std::lock_guard<std::mutex> locker(monitor.m_servicesMutex);
    for (auto it = monitor.m_subscibers.begin(); it != monitor.m_subscibers.end(); it++)
    {
        auto callback = (*it).first;
        auto param = (*it).second;
        callback(notification.dwNotificationTriggered, serviceContext.name.c_str(), serviceContext.status, notification.ServiceStatus, param);
        serviceContext.status = notification.ServiceStatus;
    }

    if (!serviceContext.handle)
        return;

    auto errorCode = ::NotifyServiceStatusChangeW(
                        serviceContext.handle,
                        s_serviceNotifyMask,
                        &notification
                    );
    if (errorCode == ERROR_SERVICE_MARKED_FOR_DELETE)
    {
        CloseServiceHandle(serviceContext.handle);
        serviceContext.handle = NULL;
    }

    if (errorCode != ERROR_SUCCESS && errorCode != ERROR_SERVICE_MARKED_FOR_DELETE)
        serviceContext.registred = false;
}

void ServicesMonitor::InstallServicesNotification(void* parameter)
{
    auto monitor = reinterpret_cast<ServicesMonitor*>(parameter);

    std::lock_guard<std::mutex> locker(monitor->m_servicesMutex);

    for (auto it = monitor->m_services.begin(); it != monitor->m_services.end(); it++)
    {
        if (!it->second.registred)
        {
            auto& notification = it->second.context.notification;

            notification.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
            notification.pfnNotifyCallback = ServiceNotificationDispatcherRoutine;
            notification.pContext = &it->second.context;
            notification.dwNotificationStatus = ERROR_SUCCESS;
            notification.dwNotificationTriggered = ERROR_SUCCESS;
            notification.pszServiceNames = ERROR_SUCCESS;

            it->second.context.monitor = monitor;
            it->second.context.serviceContext = &it->second;

            auto errorCode = NotifyServiceStatusChangeW(
                                it->second.handle,
                                s_serviceNotifyMask,
                                &notification
                            );
            if (errorCode == ERROR_SUCCESS)
                it->second.registred = true;
            else
            {
                std::cout << "ServicesMonitor::InstallServicesNotification -> NotifyServiceStatusChangeW() failed with code: " << errorCode << " ";
                std::wcout << it->first.c_str() << std::endl;
            }
        }
    }
}

void CALLBACK ServicesMonitor::SCMNotificationDispatcherRoutine(PVOID parameter)
{
    auto& context = *(ManagerNotificationContext*)parameter;
    auto& notification = context.notification;
    auto& monitor = *context.monitor;
    auto clearServiceName = CleanServiceName(notification.pszServiceNames);
    bool deleted = ((notification.dwNotificationTriggered & SERVICE_NOTIFY_DELETED) != 0);

    context.active = false;

    try
    {
        if (deleted)
            monitor.RemoveService(clearServiceName);
        else
            monitor.InsertService(clearServiceName, notification.ServiceStatus);
    }
    catch (std::exception& e)
    {
        std::wcout << L"ServicesMonitor::SCMNotificationDispatcherRoutine: unhandled exception for service '" << clearServiceName << "' : ";
        std::cout << e.what() << std::endl;
    }

    for (auto it = monitor.m_subscibers.begin(); it != monitor.m_subscibers.end(); it++)
    {
        auto callback = (*it).first;
        auto param = (*it).second;
        callback(notification.dwNotificationTriggered, clearServiceName, notification.ServiceStatus, notification.ServiceStatus, param);
    }

    context.active = true;
    auto errorCode = NotifyServiceStatusChangeW(
                        monitor.m_scManager,
                        SERVICE_NOTIFY_CREATED | SERVICE_NOTIFY_DELETED,
                        &notification
                    );
    if (errorCode != ERROR_SUCCESS)
    {
        context.active = false;
        std::cout << "ServicesMonitor::SCMNotificationDispatcherRoutine -> NotifyServiceStatusChangeW() failed with code: " << errorCode << std::endl;
    }
}

void ServicesMonitor::InstallSCMNotification(void* parameter)
{
    auto monitor = reinterpret_cast<ServicesMonitor*>(parameter);
    auto& notification = monitor->m_notification.notification;

    if (monitor->m_notification.active)
        return;

    notification.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
    notification.pfnNotifyCallback = SCMNotificationDispatcherRoutine;
    notification.pContext = 0;
    notification.dwNotificationStatus = ERROR_SUCCESS;
    notification.dwNotificationTriggered = ERROR_SUCCESS;
    notification.pszServiceNames = ERROR_SUCCESS;
    monitor->m_notification.monitor = monitor;
    monitor->m_notification.active = true;

    auto errorCode = NotifyServiceStatusChangeW(monitor->m_scManager, SERVICE_NOTIFY_CREATED | SERVICE_NOTIFY_DELETED, &notification);
    if (errorCode != ERROR_SUCCESS)
        std::cout << "ServicesMonitor::InstallSCMNotification -> NotifyServiceStatusChangeW() failed with code: " << errorCode << std::endl;
}
