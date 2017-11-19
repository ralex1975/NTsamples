#pragma once

#include <Windows.h>
#include <string>
#include <queue>
#include <list>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <memory>

struct ThrowSystemError
{
    ThrowSystemError(DWORD error, const char* message);
};

class BaseMonitorDispatcher
{
protected:

    enum MonitorNotifications
    {
        StopNotification,
        CallbackNotification,
        PauseProcessingNotification,
        MaxNotification
    };

    typedef void(*MonitorWorkCallback)(void* parameter);

private:

    enum MonitorStates
    {
        MonitorStopped,
        MonitorStarted,
        MonitorTerminating
    };

    MonitorStates m_monitorState;

    HANDLE m_dispatcherStartStopEvent;
    HANDLE m_dispatcherLockEvent;
    HANDLE m_dispatcherAcceptPauseEvent;
    HANDLE m_controlEvents[MaxNotification];

    std::shared_ptr<std::thread> m_dispatcherThread;

    typedef std::pair<MonitorWorkCallback, void*> WorkCallbackItem;
    std::queue< WorkCallbackItem, std::list<WorkCallbackItem> > m_dispatchCallbacks;

    std::mutex m_controlMutex;
    std::mutex m_dispatcherPauseMutex;
    std::mutex m_callbacksMutex;

    void ReleaseResources();

    void UnlockDispatcher();

    void SetMonitorState(MonitorStates state);
    MonitorStates GetMonitorState();

    void NotifyDispatcher(MonitorNotifications notification);

    static DWORD CALLBACK NotificationDispatcherRoutine(PVOID parameter);
    void NotificationDispatcher();

protected:

    BaseMonitorDispatcher();
    ~BaseMonitorDispatcher();

    void StartMonitor();
    void StopMonitor();

    void PauseDispatcher();
    void ResumeDispatcher();

    void PushCallback(MonitorWorkCallback callback, void* parameter);

    static const wchar_t* CleanServiceName(const wchar_t* serviceName);
    
    static HANDLE AllocateEvent(bool manual = false, bool initial = false);

};

class ServicesMonitor : public BaseMonitorDispatcher
{
public:

    typedef void(*ServiceNotificationCallback)(
        DWORD notification, 
        const wchar_t* serviceName, 
        const SERVICE_STATUS_PROCESS& oldStatus, 
        const SERVICE_STATUS_PROCESS& newStatus, 
        void* parameter
    );

private:

    struct ServiceContext;

    struct NotificationContext
    {
        SERVICE_NOTIFYW notification;
        ServicesMonitor* monitor;
        ServiceContext* serviceContext;
    };

    struct ServiceContext
    {
        bool registred;
        SC_HANDLE handle;
        std::wstring name;
        SERVICE_STATUS_PROCESS status;
        NotificationContext context;
    };

    struct ManagerNotificationContext
    {
        SERVICE_NOTIFYW notification;
        ServicesMonitor* monitor;
        bool active;
    };

    ManagerNotificationContext m_notification;

    std::mutex m_controlMutex;

    typedef std::pair<ServiceNotificationCallback, void*> SubscriberContext;
    std::set<SubscriberContext> m_subscibers;

    std::mutex m_servicesMutex;
    std::map<std::wstring, ServiceContext> m_services;

    static const DWORD s_serviceNotifyMask;

    void EnumAndInsertServices();

    static void CALLBACK ServiceNotificationDispatcherRoutine(PVOID parameter);
    static void InstallServicesNotification(void* parameter);

    static void CALLBACK SCMNotificationDispatcherRoutine(PVOID parameter);
    static void InstallSCMNotification(void* parameter);

protected:

    bool m_monitoringStarted;

    SC_HANDLE m_scManager;

public:

    ServicesMonitor();
    ~ServicesMonitor();

    void InsertService(const wchar_t* serviceName, const SERVICE_STATUS_PROCESS& serviceStatus);
    void RemoveService(const wchar_t* serviceName);

    void Subscribe(ServiceNotificationCallback callback, void* parameter);
    void Unsubscribe(ServiceNotificationCallback callback);

    void StartMonitoring();
    void StopMonitoring();

};
