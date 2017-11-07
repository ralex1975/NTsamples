#include <Windows.h>
#include <stdio.h>
#include <iostream>
#include <sstream>
#include "ServicesMonitor.h"

std::wstring ServiceNotificationToUnicode(DWORD notifications)
{
	std::wstringstream str;

	if (notifications & SERVICE_NOTIFY_CREATED)
		str << L"CREATED";

	if (notifications & SERVICE_NOTIFY_CONTINUE_PENDING)
		str << L"CONTINUE_PENDING";

	if (notifications & SERVICE_NOTIFY_DELETE_PENDING)
		str << L"DELETE_PENDING";

	if (notifications & SERVICE_NOTIFY_DELETED)
		str << L"DELETED";

	if (notifications & SERVICE_NOTIFY_PAUSE_PENDING)
		str << L"PAUSE_PENDING";

	if (notifications & SERVICE_NOTIFY_PAUSED)
		str << L"PAUSED";

	if (notifications & SERVICE_NOTIFY_RUNNING)
		str << L"RUNNING";

	if (notifications & SERVICE_NOTIFY_START_PENDING)
		str << L"START_PENDING";

	if (notifications & SERVICE_NOTIFY_STOP_PENDING)
		str << L"STOP_PENDING";

	if (notifications & SERVICE_NOTIFY_STOPPED)
		str << L"STOPPED";

	return str.str();
}

const std::wstring ServiceTypeToUnicode(DWORD type)
{
	std::wstringstream str;

	if (type & SERVICE_KERNEL_DRIVER)
		str << L" KERNEL_DRIVER";

	if (type & SERVICE_FILE_SYSTEM_DRIVER)
		str << L" FILE_SYSTEM_DRIVER";

	if (type & SERVICE_ADAPTER)
		str << L" ADAPTER";

	if (type & SERVICE_RECOGNIZER_DRIVER)
		str << L" RECOGNIZER_DRIVER";

	if (type & SERVICE_WIN32_OWN_PROCESS)
		str << L" WIN32_OWN_PROCESS";

	if (type & SERVICE_WIN32_SHARE_PROCESS)
		str << L" WIN32_SHARE_PROCESS";

	if (type & SERVICE_INTERACTIVE_PROCESS)
		str << L" INTERACTIVE_PROCESS";

	str << L" (0x" << std::hex << type << std::dec << L")";

	return str.str();
}

const std::wstring ServiceStateTypeToUnicode(DWORD state)
{
	std::wstringstream str;

	switch (state)
	{
	case SERVICE_STOPPED:
		str << L"STOPPED";
		break;
	case SERVICE_START_PENDING:
		str << L"START_PENDING";
		break;
	case SERVICE_STOP_PENDING:
		str << L"STOP_PENDING";
		break;
	case SERVICE_RUNNING:
		str << L"RUNNING";
		break;
	case SERVICE_CONTINUE_PENDING:
		str << L"CONTINUE_PENDING";
		break;
	case SERVICE_PAUSE_PENDING:
		str << L"PAUSE_PENDING";
		break;
	case SERVICE_PAUSED:
		str << L"PAUSED";
		break;
	default:
		str << L"Unknown";
		break;
	}

	str << L" (0x" << std::hex << state << std::dec << L")";

	return str.str();
}

const std::wstring ServiceControlStateToUnicode(DWORD control)
{
	std::wstringstream str;

	if (!control)
	{
		str << L"0";
		return str.str();
	}

	if (control & SERVICE_ACCEPT_STOP)
	{
		str << L"ACCEPT_STOP ";
		control &= ~SERVICE_ACCEPT_STOP;
	}

	if (control & SERVICE_ACCEPT_PAUSE_CONTINUE)
	{
		str << L"ACCEPT_PAUSE_CONTINUE ";
		control &= ~SERVICE_ACCEPT_PAUSE_CONTINUE;
	}

	if (control & SERVICE_ACCEPT_SHUTDOWN)
	{
		str << L"ACCEPT_SHUTDOWN ";
		control &= ~SERVICE_ACCEPT_SHUTDOWN;
	}

	if (control & SERVICE_ACCEPT_PARAMCHANGE)
	{
		str << L"ACCEPT_PARAMCHANGE ";
		control &= ~SERVICE_ACCEPT_PARAMCHANGE;
	}

	if (control & SERVICE_ACCEPT_NETBINDCHANGE)
	{
		str << L"ACCEPT_NETBINDCHANGE ";
		control &= ~SERVICE_ACCEPT_NETBINDCHANGE;
	}

	if (control & SERVICE_ACCEPT_HARDWAREPROFILECHANGE)
	{
		str << L"ACCEPT_HARDWAREPROFILECHANGE ";
		control &= ~SERVICE_ACCEPT_HARDWAREPROFILECHANGE;
	}

	if (control & SERVICE_ACCEPT_POWEREVENT)
	{
		str << L"ACCEPT_POWEREVENT ";
		control &= ~SERVICE_ACCEPT_POWEREVENT;
	}

	if (control & SERVICE_ACCEPT_SESSIONCHANGE)
	{
		str << L"ACCEPT_SESSIONCHANGE ";
		control &= ~SERVICE_ACCEPT_SESSIONCHANGE;
	}

	if (control & SERVICE_ACCEPT_PRESHUTDOWN)
	{
		str << L"ACCEPT_PRESHUTDOWN ";
		control &= ~SERVICE_ACCEPT_PRESHUTDOWN;
	}

	if (control & SERVICE_ACCEPT_TIMECHANGE)
	{
		str << L"ACCEPT_TIMECHANGE ";
		control &= ~SERVICE_ACCEPT_TIMECHANGE;
	}

	if (control & SERVICE_ACCEPT_TRIGGEREVENT)
	{
		str << L"ACCEPT_TRIGGEREVENT ";
		control &= ~SERVICE_ACCEPT_TRIGGEREVENT;
	}

	if (control & /*SERVICE_ACCEPT_USERMODEREBOOT*/0x00000800)
	{
		str << L"ACCEPT_USERMODEREBOOT ";
		control &= ~0x00000800;
	}

	if (control)
		str << L"Unknown (0x" << std::hex << control << std::dec << L")";

	return str.str();
}

void PrintStatusChanges(const SERVICE_STATUS_PROCESS& oldServiceStatus, const SERVICE_STATUS_PROCESS& newServiceStatus)
{
	if (oldServiceStatus.dwServiceType != newServiceStatus.dwServiceType)
		std::wcout << L"  Type -> from " << ServiceTypeToUnicode(oldServiceStatus.dwServiceType)
			<< " to " << ServiceTypeToUnicode(newServiceStatus.dwServiceType) << std::endl;

	if (oldServiceStatus.dwCurrentState != newServiceStatus.dwCurrentState)
		std::wcout << L"  Current State -> from " << ServiceStateTypeToUnicode(oldServiceStatus.dwCurrentState)
			<< " to " << ServiceStateTypeToUnicode(newServiceStatus.dwCurrentState) << std::endl;

	if (oldServiceStatus.dwControlsAccepted != newServiceStatus.dwControlsAccepted)
		std::wcout << L"  Accepted Controls -> from " << ServiceControlStateToUnicode(oldServiceStatus.dwControlsAccepted)
			<< " to " << ServiceControlStateToUnicode(newServiceStatus.dwControlsAccepted) << std::endl;
	
	if (oldServiceStatus.dwWin32ExitCode != newServiceStatus.dwWin32ExitCode)
		std::wcout << std::hex << L"  Win32 Exit Code -> from 0x" << oldServiceStatus.dwWin32ExitCode
			<< " to 0x" << newServiceStatus.dwWin32ExitCode << std::dec << std::endl;
	
	if (oldServiceStatus.dwServiceSpecificExitCode != newServiceStatus.dwServiceSpecificExitCode)
		std::wcout << std::hex << L"  Specific Exit Code -> from 0x" << oldServiceStatus.dwServiceSpecificExitCode
			<< " to 0x" << newServiceStatus.dwServiceSpecificExitCode << std::dec << std::endl;

	if (oldServiceStatus.dwCheckPoint != newServiceStatus.dwCheckPoint)
		std::wcout << std::hex << L"  Check Point -> from 0x" << oldServiceStatus.dwCheckPoint
			<< " to 0x" << newServiceStatus.dwCheckPoint << std::dec << std::endl;

	if (oldServiceStatus.dwWaitHint != newServiceStatus.dwWaitHint)
		std::wcout << std::hex << L"  Wait Hint -> from 0x" << oldServiceStatus.dwWaitHint
			<< " to 0x" << newServiceStatus.dwWaitHint << std::dec << std::endl;

	if (oldServiceStatus.dwProcessId != newServiceStatus.dwProcessId)
		std::wcout << L"  Process ID -> from " << oldServiceStatus.dwProcessId 
			<< " to " << newServiceStatus.dwProcessId << std::endl;

	if (oldServiceStatus.dwServiceFlags != newServiceStatus.dwServiceFlags)
		std::wcout << L"  Flags -> from 0x" << oldServiceStatus.dwServiceFlags 
			<< " to 0x" << newServiceStatus.dwServiceFlags << std::endl;
}

void ServiceNotificationCallback(
	DWORD notification,
	const wchar_t* serviceName,
	const SERVICE_STATUS_PROCESS& oldStatus,
	const SERVICE_STATUS_PROCESS& newStatus,
	void* parameter)
{
	std::wcout << L"[" << ServiceNotificationToUnicode(notification).c_str() << L"] " << serviceName << L" " << std::endl;
	PrintStatusChanges(oldStatus, newStatus);
}

void SCMNotificationCallback(DWORD notification, const wchar_t* serviceName, const SERVICE_STATUS_PROCESS& serviceStatus, void* parameter)
{
	std::wcout << L"[" << ServiceNotificationToUnicode(notification).c_str() << L"] " << serviceName << L" " << std::endl;
}

int wmain(int argc, wchar_t* argv[])
{
	try
	{
		ServiceControlManagerMonitor scmMonitor;
		scmMonitor.Subscribe(ServiceNotificationCallback, NULL);
		scmMonitor.SubscribeSCM(SCMNotificationCallback, NULL);
		scmMonitor.StartMonitoring();

		printf("Press ENTER to exit\n");
		getchar();
	}
	catch (std::system_error& e)
	{
		std::cout << "System exception: " << e.what() << " (code: " << e.code() << ")" << std::endl;
	}
	catch (std::exception& e)
	{
		std::cout << "Unhandled exception: " << e.what() << std::endl;
	}

	return 0;
}
