/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"
#include "Entrypoints.h"
#include "ConsoleArguments.hpp"

#include "DeviceHandle.h"
#include "IoThread.h"

#include "winbasep.h"

HRESULT Entrypoints::StartConsoleForServerHandle(_In_ HANDLE const ServerHandle)
{
    return ConsoleCreateIoThreadLegacy(ServerHandle);
}

// this function has unreachable code due to its unusual lifetime. We
// disable the warning about it here.
#pragma warning(push)
#pragma warning(disable:4702)

HRESULT Entrypoints::StartConsoleForCmdLine(_In_ PCWSTR pwszCmdLine)
{
    ConsoleArguments arguments = ConsoleArguments(pwszCmdLine);
    RETURN_IF_FAILED(arguments.ParseCommandline());
    // Create a scope because we're going to exit thread if everything goes well.
    // This scope will ensure all C++ objects and smart pointers get a chance to destruct before ExitThread is called.
    {
        // TODO:MSFT:13271366 use the arguments from the commandline to determine if we need 
        //  to create the server handle or not.

        // Create the server and reference handles and create the console object.
        wil::unique_handle ServerHandle;
        RETURN_IF_NTSTATUS_FAILED(DeviceHandle::CreateServerHandle(ServerHandle.addressof(), FALSE));

        wil::unique_handle ReferenceHandle;
        RETURN_IF_NTSTATUS_FAILED(DeviceHandle::CreateClientHandle(ReferenceHandle.addressof(),
                                                                   ServerHandle.get(),
                                                                   L"\\Reference",
                                                                   FALSE));

        RETURN_IF_NTSTATUS_FAILED(Entrypoints::StartConsoleForServerHandle(ServerHandle.get()));

        // If we get to here, we have transferred ownership of the server handle to the console, so release it.
        // Keep a copy of the value so we can open the client handles even though we're no longer the owner.
        HANDLE const hServer = ServerHandle.release();

        // Now that the console object was created, we're in a state that lets us
        // create the default io objects.
        wil::unique_handle ClientHandle[3];

        // Input
        RETURN_IF_NTSTATUS_FAILED(DeviceHandle::CreateClientHandle(ClientHandle[0].addressof(),
                                                                   hServer,
                                                                   L"\\Input",
                                                                   TRUE));

        // Output
        RETURN_IF_NTSTATUS_FAILED(DeviceHandle::CreateClientHandle(ClientHandle[1].addressof(),
                                                                   hServer,
                                                                   L"\\Output",
                                                                   TRUE));

        // Error is a copy of Output
        RETURN_IF_WIN32_BOOL_FALSE(DuplicateHandle(GetCurrentProcess(),
                                                   ClientHandle[1].get(),
                                                   GetCurrentProcess(),
                                                   ClientHandle[2].addressof(),
                                                   0,
                                                   TRUE,
                                                   DUPLICATE_SAME_ACCESS));

        // Create the child process. We will temporarily overwrite the values in the
        // PEB to force them to be inherited.

        STARTUPINFOEX StartupInformation = { 0 };
        StartupInformation.StartupInfo.cb = sizeof(STARTUPINFOEX);
        StartupInformation.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        StartupInformation.StartupInfo.hStdInput = ClientHandle[0].get();
        StartupInformation.StartupInfo.hStdOutput = ClientHandle[1].get();
        StartupInformation.StartupInfo.hStdError = ClientHandle[2].get();

        // Get the parent startup info for this process. It might contain LNK data we need to pass to the child.
        {
            STARTUPINFO HostStartupInfo = { 0 };
            HostStartupInfo.cb = sizeof(STARTUPINFO);
            GetStartupInfoW(&HostStartupInfo);

            // If we were started with Title is Link Name, then pass the flag and the link name down to the child.
            if (IsFlagSet(HostStartupInfo.dwFlags, STARTF_TITLEISLINKNAME))
            {
                StartupInformation.StartupInfo.lpTitle = HostStartupInfo.lpTitle;
                StartupInformation.StartupInfo.dwFlags |= STARTF_TITLEISLINKNAME;
            }
        }

        // Create the extended attributes list that will pass the console server information into the child process.

        // Call first time to find size
        SIZE_T AttributeListSize;
        InitializeProcThreadAttributeList(NULL,
                                          2,
                                          0,
                                          &AttributeListSize);

        // Alloc space
        wistd::unique_ptr<BYTE[]> AttributeList = wil::make_unique_nothrow<BYTE[]>(AttributeListSize);
        RETURN_IF_NULL_ALLOC(AttributeList);

        StartupInformation.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(AttributeList.get());

        // Call second time to actually initialize space.
        RETURN_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(StartupInformation.lpAttributeList,
                                                                     2, // This represents the length of the list. We will call UpdateProcThreadAttribute twice so this is 2.
                                                                     0,
                                                                     &AttributeListSize));
        // Set cleanup data for ProcThreadAttributeList when successful.
        auto CleanupProcThreadAttribute = wil::ScopeExit([&]
        {
            DeleteProcThreadAttributeList(StartupInformation.lpAttributeList);
        });

        RETURN_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(StartupInformation.lpAttributeList,
                                                             0,
                                                             PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE,
                                                             ReferenceHandle.addressof(),
                                                             sizeof(HANDLE),
                                                             NULL,
                                                             NULL));

        // UpdateProcThreadAttributes wants this as a bare array of handles and doesn't like our smart structures,
        // so set it up for its use.
        HANDLE HandleList[3];
        HandleList[0] = StartupInformation.StartupInfo.hStdInput;
        HandleList[1] = StartupInformation.StartupInfo.hStdOutput;
        HandleList[2] = StartupInformation.StartupInfo.hStdError;

        RETURN_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(StartupInformation.lpAttributeList,
                                                                0,
                                                                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                                &HandleList[0],
                                                                sizeof HandleList,
                                                                NULL,
                                                                NULL));

        ////////////////////////////////////////////////////////////////////////
        // TEMP: Use results of arg parsing
        // Make sure to store the wstring return value in a local 
        //    otherwise .c_str() will not return the right value.
        std::wstring clientCmd = arguments.GetClientCommandline();
        const wchar_t* pwszClientCmdLine = clientCmd.c_str();
        if (arguments.IsUsingVtPipe())
        {
            RETURN_IF_FAILED(UseVtPipe(arguments.GetVtInPipe(), arguments.GetVtOutPipe(), arguments.GetVtMode()));
        }
        ////////////////////////////////////////////////////////////////////////
        
        // We have to copy the command line string we're given because CreateProcessW has to be called with mutable data.
        if (wcslen(pwszClientCmdLine) == 0)
        {
            // If they didn't give us one, just launch cmd.exe.
            pwszClientCmdLine = L"%WINDIR%\\system32\\cmd.exe";
        }

        // Expand any environment variables present in the command line string.
        // - Get needed size
        DWORD cchCmdLineExpanded = ExpandEnvironmentStringsW(pwszClientCmdLine, nullptr, 0);
        RETURN_LAST_ERROR_IF(0 == cchCmdLineExpanded);

        // - Allocate space to hold result
        wistd::unique_ptr<wchar_t[]> CmdLineMutable = wil::make_unique_nothrow<wchar_t[]>(cchCmdLineExpanded);
        RETURN_IF_NULL_ALLOC(CmdLineMutable);

        // - Expand string into allocated space
        RETURN_LAST_ERROR_IF(0 == ExpandEnvironmentStringsW(pwszClientCmdLine, CmdLineMutable.get(), cchCmdLineExpanded));

        // Call create process
        wil::unique_process_information ProcessInformation;
        RETURN_IF_WIN32_BOOL_FALSE(CreateProcessW(NULL,
                                                  CmdLineMutable.get(),
                                                  NULL,
                                                  NULL,
                                                  TRUE,
                                                  EXTENDED_STARTUPINFO_PRESENT,
                                                  NULL,
                                                  NULL,
                                                  &StartupInformation.StartupInfo,
                                                  ProcessInformation.addressof()));
    }

    // Exit the thread so the CRT won't clean us up and kill. The IO thread owns the lifetime now.
    ExitThread(S_OK);

    // We won't hit this. The ExitThread above will kill the caller at this point.
    assert(false);
    return S_OK;
}
#pragma warning(pop)
