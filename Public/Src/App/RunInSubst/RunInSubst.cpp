// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

// This is an implementation of a file locking based protocol for subst drives and executing
// an process, passed on the command line.
// The command line interface is:
// RunInSubst <Drive>=<path-tosubst>* <process-to-execute> <command-parameters>
// The locking is done in always the same order, so deadlocks are avoided - see pOrderedSubstList.
// The way it works is the process gets a lock on a file in the drive to subst - the process opens a predefined
// file (where diagnostics are logged as well) with exclusive write and shared read access. If another process
// tries to open the same file, it fails with sharing validation. The second process will wait for the first 
// process to close the file and will get the lock once that happens.
//

#pragma warning( disable: 4820 ) // Shut-off padding warnings.
#include "stdafx.h"

#define RUN_IN_SUBST_TIMEOUT 5000
#define NUMBER_DEFINABLE_SUBST 26
#define RUN_IN_SUBST_VERSION L"1.0"
#define MIN_SUBST_LENGTH 3
#define SUBST_START_OFFSET 2
#define SUBST_SOURCE_LENGTH 65536
#define GET_PATH_TARGET_OFFSET  8
#define RUN_IN_SUBST_VERBOSE L"RUN_IN_SUBST_VERBOSE"
#define RUN_IN_SUBST_VERBOSE_BUFF_SIZE 2
#define MAPPED_PATH_STRING L"\\??\\"
#define SUBST_FILE_NAME L".SubstLock"

// warning C6001: Using uninitialized memory '*substFileLockHandle'.
#pragma warning( disable : 6001 )

#pragma warning( push )
// warning C26409: Avoid calling new and delete explicitly, use std::make_unique<T> instead (r.11).
#pragma warning( disable : 26409 )
// Subst target and source node.
typedef struct _tagSubstNode
{
    _tagSubstNode() noexcept
    {
        szDriveLetter = L'\0';
        szMappedPath = nullptr;
        szSourceDirectory = nullptr;
        hLockFile = INVALID_HANDLE_VALUE;
    }

    _tagSubstNode(TCHAR drive, const TCHAR* srcDir)
    {
        szDriveLetter = drive;
        const auto srcLen = wcslen(srcDir) + 1;
        szSourceDirectory = new TCHAR[srcLen];
        wcscpy_s(szSourceDirectory, srcLen, srcDir);
        szMappedPath = nullptr;
        hLockFile = INVALID_HANDLE_VALUE;
    }

    TCHAR szDriveLetter;
    TCHAR* szSourceDirectory;
    TCHAR* szMappedPath;
    HANDLE hLockFile;
} SUBST_NODE, * PSUBST_NODE;
#pragma warning( pop )

// List of parsed subst target and source.
typedef struct _tagSubstListNode
{
    _tagSubstListNode() noexcept
    {
        pNext = nullptr;
        pData = nullptr;
    }

    PSUBST_NODE pData;
    _tagSubstListNode* pNext;
} SUBST_LIST_NODE, *PSUBST_LIST_NODE;

static bool g_isVerbose = false;

template <typename... Args>
static void printVerbose(PCWSTR format, Args&&... args)
{
    if (g_isVerbose)
    {
        std::wstring formatted = std::vformat(format, std::make_wformat_args(args...));
        wprintf(L"Verbose: %s\r\n", formatted.c_str());
    }
}

static void printUsage() noexcept
{
    wprintf(L"%s %s\r\n", L"Microsoft(R) RunInSubst Build Tool.Version: ", RUN_IN_SUBST_VERSION);
    wprintf(L"Copyright(C) Microsoft Corporation.All rights reserved.\r\n\r\n");

    wprintf(L"Usage:\r\n");
    wprintf(L"RunInSubst [<target drive>=<source location> ...] <executable-to-start> <arguments-for-the-executable-to-start>\r\n");
}

#pragma warning( push )
// warning C26409: Avoid calling new and delete explicitly, use std::make_unique<T> instead (r.11).
// warning C6011: Dereferencing NULL pointer 'ppSubstList'. : Lines: 99, 100, 101, 103
// warning C6011: Dereferencing NULL pointer 'ppOrderedSubstList'. : Lines: 99, 100, 101, 103, 106
#pragma warning( disable : 26409 6011 )
// Initializes the state of the application.
static void InitializeState(PSUBST_LIST_NODE* ppSubstList, PSUBST_NODE** ppOrderedSubstList, int* pExecutableToRunIndex)
{
    assert(ppSubstList != nullptr);
    assert(ppOrderedSubstList != nullptr);
    assert(pExecutableToRunIndex != nullptr);

    *ppSubstList = nullptr;

    // Array of ordered subst target and lists.
    *ppOrderedSubstList = new PSUBST_NODE[NUMBER_DEFINABLE_SUBST];
    ZeroMemory(*ppOrderedSubstList, sizeof(PSUBST_NODE) * NUMBER_DEFINABLE_SUBST);

    *pExecutableToRunIndex = -1;
}
#pragma warning( pop )

#pragma warning( push )
// warning C26446: Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
// warning C26472: Don't use a static_cast for arithmetic conversions. Use brace initialization, gsl::narrow_cast or gsl::narrow (type.1).
#pragma warning( disable : 26446 26472 )
std::string ToUtf8(const std::wstring& s)
{
    std::string utf8;
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.length()), NULL, 0, NULL, NULL);
    if (len > 0)
    {
        utf8.resize(static_cast<size_t>(len));
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.length()), &utf8[0], len, NULL, NULL);
    }
    return utf8;
}
#pragma warning( pop )

#pragma warning( push )
// warning C26472: Don't use a static_cast for arithmetic conversions. Use brace initialization, gsl::narrow_cast or gsl::narrow (type.1).
#pragma warning( disable : 26472 )
// Logs string to a file.
template <typename... Args>
static void LogToFile(PSUBST_NODE pSubstNode, PCWSTR format, Args&&... args)
{
    if (pSubstNode != nullptr && pSubstNode->hLockFile != INVALID_HANDLE_VALUE)
    {
        char timeDateString[1024];
        time_t timer;
        tm tm_info;

        time(&timer);
        localtime_s(&tm_info, &timer);

        strftime(timeDateString, 1024, "%Y-%m-%d %H:%M:%S - ", &tm_info);

        std::wstring bufferForArgs = std::vformat(format, std::make_wformat_args(args...));
        bufferForArgs.append(L"\r\n");
        std::string asciiBuffer = ToUtf8(bufferForArgs);

        if (pSubstNode->hLockFile != INVALID_HANDLE_VALUE)
        {
            WriteFile(pSubstNode->hLockFile, timeDateString, strlen(timeDateString) * sizeof(char), nullptr, nullptr);
            WriteFile(pSubstNode->hLockFile, asciiBuffer.c_str(), asciiBuffer.length(), nullptr, nullptr);
        }
    }
}
#pragma warning( pop )

#pragma warning( push )
// warning C26485: Expression 'argv': No array to pointer decay (bounds.3).
// warning C26472: Don't use a static_cast for arithmetic conversions. Use brace initialization, gsl::narrow_cast or gsl::narrow (type.1).
// warning C26446: Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
// warning C26481: Don't use pointer arithmetic. Use span instead (bounds.1).
// warning C26409: Avoid calling new and delete explicitly, use std::make_unique<T> instead (r.11).
// warning C26400: Do not assign the result of an allocation or a function call with an owner<T> return value to a raw pointer, use owner<T> instead (i.11).
// warning C6011: Dereferencing NULL pointer 'ppSubstList'
// warning C26401: Do not delete a raw pointer that is not an owner<T> (i.11).
#pragma warning( disable : 26485 26472 26446 26481 26409 26400 6011 26401 )

// Finds and sets the state for subst targets and sources.
// Returns whether the app should exit. True is the app should exit right away. False, the app should continue;
static bool ParseSubstSourcesAndTargets(int argc, _TCHAR* argv[], PSUBST_LIST_NODE* ppSubstList, PSUBST_NODE* pOrderedSubstList, int* pExecutableToRunIndex)
{
    assert(argv != nullptr);
    assert(ppSubstList != nullptr);
    assert(pOrderedSubstList != nullptr);
    assert(pExecutableToRunIndex != nullptr);

    PSUBST_LIST_NODE currentNode = nullptr;
    for (int i = 1; i < argc; i++)
    {
        // Check to see if this is a mapping. (Second character a '=' and first a letter.)
        // Also, the length of the subst string should be longer than MIN_SUBST_LENGTH.
        if (wcsstr(argv[i], L"=") != (argv[i] + 1) ||
            !isalpha(argv[i][0]) ||
            wcslen(argv[i]) < MIN_SUBST_LENGTH)
        {
            *pExecutableToRunIndex = i;
            return false;
        }

        const TCHAR substTarget = static_cast<TCHAR>(::_totupper(argv[i][0])); // Make it uppercase.
        std::basic_string<TCHAR> substSource(&argv[i][SUBST_START_OFFSET]);

        std::transform(substSource.begin(), substSource.end(), substSource.begin(),
            [](TCHAR c) noexcept
            {
                return static_cast<TCHAR>(::_totlower(c));
            });

        // make sure there is a trailing '\\'.
        if (substSource.back() != L'\\')
        {
            substSource.push_back(L'\\');
        }

        int substTargetValue = static_cast<int>(substTarget);
        // The drive letter can be A-Z only.
        if (substTargetValue < 65 || substTargetValue > 90)
        {
            TCHAR dl[2];
            dl[0] = substTarget;
            dl[1] = L'\0';
            wprintf(L"Error: Invalid target drive letter - %s. Allowed drive letters A-Z.\r\n", dl);
            return true;
        }

        // If there was a map entry for this drive, just update it.
        if (pOrderedSubstList[substTargetValue - 65] != nullptr)
        {
            delete[] pOrderedSubstList[substTargetValue - 65]->szSourceDirectory;
            pOrderedSubstList[substTargetValue - 65]->szSourceDirectory = new TCHAR[substSource.length() + 1];

            wcscpy_s(pOrderedSubstList[substTargetValue - 65]->szSourceDirectory, substSource.length(), substSource.c_str());
            continue;
        }

        const DWORD srcAttr = GetFileAttributes(substSource.c_str());
        if (srcAttr == INVALID_FILE_ATTRIBUTES)
        {
            wprintf(L"Warning: The local location %s is invalid.\r\n", substSource.c_str());
            return true;;
        }

        if ((srcAttr & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            wprintf(L"Warning: The local location %s is invalid. It should be a directory.\r\n", substSource.c_str());
            return true;;
        }

        PSUBST_NODE pSubstNode = new SUBST_NODE(substTarget, substSource.c_str());
        PSUBST_LIST_NODE pSubstListNode = new SUBST_LIST_NODE();
        pSubstListNode->pData = pSubstNode;

        if (currentNode == nullptr)
        {
            // First iteration.
            *ppSubstList = currentNode = pSubstListNode;
        }
        else
        {
            currentNode->pNext = pSubstListNode;
            currentNode = pSubstListNode;
        }

        pOrderedSubstList[substTargetValue - 65] = pSubstNode;
    }

    return false;
}
#pragma warning( pop )

#pragma warning( push )
// warning C6386: Buffer overrun while writing to 'chBuf'.
// warning C26446: Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
// warning C26401: Do not delete a raw pointer that is not an owner<T> (i.11).
// warning C26414: Move, copy, reassign or reset a local smart pointer 'chBuf' / 'mappedPath' (r.5).
// warning C26481: Don't use pointer arithmetic. Use span instead (bounds.1).
// warning C26485: Expression 'dl': No array to pointer decay (bounds.3).
// warning C26472: Don't use a static_cast for arithmetic conversions. Use brace initialization, gsl::narrow_cast or gsl::narrow (type.1).
// warning C26409: Avoid calling new and delete explicitly, use std::make_unique<T> instead (r.11).
#pragma warning( disable : 6386 26446 26401 26414 26481 26485 26472 26409 )

// Gets the mapped path for each mapped drive.
// Returns 0 if successful and non-zero if failed.
static int GetMappedPaths(PSUBST_NODE* pOrderedSubstList)
{
    assert(pOrderedSubstList != nullptr);

    SECURITY_ATTRIBUTES saAttr{};
    HANDLE g_hChildStd_IN_Rd = NULL;
    HANDLE g_hChildStd_OUT_Rd = NULL;
    HANDLE g_hChildStd_OUT_Wr = NULL;

    // Set the bInheritHandle flag so pipe handles are inherited. 
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    // Create a pipe for the child process's STDOUT. 

    if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0))
    {
        wprintf(L"Error: Could not get MappedDrives: CreatePipe.\r\n");
        return 1;
    }

    // Ensure the read handle to the pipe for STDOUT is not inherited
    if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0))
    {
        wprintf(L"Error: Could not get MappedDrives: SetHandleInformation.\r\n");
        return 1;
    }

    BOOL bSuccess = FALSE;
    std::wstring substCommand(L"subst.exe");

    // Set up members of the PROCESS_INFORMATION structure
    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    // Declare process info
    STARTUPINFO siStartInfo;

    // Set up members of the STARTUPINFO structure. 
    // This structure specifies the STDIN and STDOUT handles for redirection.
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdError = g_hChildStd_OUT_Wr;
    siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
    siStartInfo.hStdInput = g_hChildStd_IN_Rd;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    // Create the child process. 
    bSuccess = CreateProcess(NULL,
        (LPWSTR)substCommand.c_str(), // command line 
        NULL,     // process security attributes 
        NULL,     // primary thread security attributes 
        TRUE,     // handles are inherited 
        0,       // creation flags 
        NULL,     // use parent's environment 
        NULL,     // use parent's current directory 
        &siStartInfo, // STARTUPINFO pointer 
        &piProcInfo); // receives PROCESS_INFORMATION

    // If an error occurs...
    if (!bSuccess)
    {
        wprintf(L"Error: Could not get MappedDrives: CreateProcess.\r\n");
        return 1;
    }

    printVerbose(L"{}", L"Start waiting for subst process in GetMappedPath to complete.");
    WaitForSingleObject(piProcInfo.hProcess, INFINITE);
    printVerbose(L"{}", L"Done waiting for subst process in GetMappedPath to complete.");

    // Close handles to the child process and its primary thread
    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);

    // Read output from the child process's pipe for STDOUT
    // Stop when there is no more data. 
    bSuccess = FALSE;

    // Close the write end of the pipe before reading from the 
    // read end of the pipe, to control child process execution.
    // The pipe is assumed to have enough buffer space to hold the
    // data the child process has already written to it.
    if (!CloseHandle(g_hChildStd_OUT_Wr))
    {
        wprintf(L"Error: Could not get MappedDrives: CloseChildHandle.\r\n");
        return 1;
    }

    DWORD dwRead = 0;
    auto chBuf = std::vector<char>(SUBST_SOURCE_LENGTH, 0);
    std::basic_string<TCHAR> mappedPath;

    while (true)
    {
        bSuccess = ReadFile(g_hChildStd_OUT_Rd, chBuf.data(), SUBST_SOURCE_LENGTH, &dwRead, NULL);

        // When using CreateProcess everything is fine here
        // When using CreateProcessWithLogonW bSuccess is FALSE and the pipe seems to be closed

        if (!bSuccess || dwRead == 0)
        {
            break;
        }

        if (dwRead == 0) break;
        chBuf[dwRead] = L'\0';

        unsigned i = 0;

        while (true)
        {
            if (chBuf[i] == L'\0')
            {
                break;
            }

            // Find the drive we are trying to map
            TCHAR driveLetter = (TCHAR)chBuf[i];

            i += GET_PATH_TARGET_OFFSET;
            mappedPath.clear();

            while (chBuf[i] != L'\r' && chBuf[i] != L'\0')
            {
                mappedPath.push_back(static_cast<TCHAR>(::tolower(chBuf[i])));
                i++;
            }

            const wchar_t* ptr = wcsstr(mappedPath.c_str(), MAPPED_PATH_STRING);
            unsigned mappedPathOffset = 0;
            if (ptr == mappedPath.c_str())
            {
                mappedPathOffset = 4;
            }

            // Make drive letter uppercase.
            driveLetter = static_cast<TCHAR>(::_totupper(driveLetter));
            int substTargetValue = static_cast<int>(driveLetter);

            // The drive letter can be A-Z only.
            if (substTargetValue < 65 || substTargetValue > 90)
            {
                TCHAR dl[2];
                dl[0] = driveLetter;
                dl[1] = L'\0';
                wprintf(L"Error: Invalid target drive letter - %s. Allowed drive letters A-Z.\r\n", dl);
                return 1;
            }

            // make sure there is a trailing '\\'.
            const size_t srcLen = wcslen(mappedPath.c_str());
            if (mappedPath.back() != L'\\')
            {
                mappedPath.push_back(L'\\');
            }

            if (pOrderedSubstList[substTargetValue - 65] != nullptr)
            {
                if (pOrderedSubstList[substTargetValue - 65]->szMappedPath != nullptr)
                {
                    delete[] pOrderedSubstList[substTargetValue - 65]->szMappedPath;
                }

                pOrderedSubstList[substTargetValue - 65]->szMappedPath = new TCHAR[srcLen + 2];
                wcscpy_s(pOrderedSubstList[substTargetValue - 65]->szMappedPath, srcLen + 2, mappedPath.c_str() + mappedPathOffset); // Skip leading "\\??\\".
            }

            if (chBuf[i] == L'\0')
            {
                break;
            }
            else
            {
                // Skip the \r char
                i++;
                // Skip the \n char
                i++;
            }
        }

        if (chBuf[i])
        {
            break;
        }
    }

    CloseHandle(g_hChildStd_OUT_Rd);

    return 0;
}
#pragma warning( pop )

#pragma warning( push )
// warning C6386: Buffer overrun while writing to 'chBuf'.
// warning C26446: Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
// warning C26401: Do not delete a raw pointer that is not an owner<T> (i.11).
// warning C26414: Move, copy, reassign or reset a local smart pointer 'chBuf' / 'mappedPath' (r.5).
// warning C26481: Don't use pointer arithmetic. Use span instead (bounds.1).
// warning C26485: Expression 'dl': No array to pointer decay (bounds.3).
// warning C26409: Avoid calling new and delete explicitly, use std::make_unique<T> instead (r.11).
// warning C26472: Don't use a static_cast for arithmetic conversions. Use brace initialization, gsl::narrow_cast or gsl::narrow (type.1).
#pragma warning( disable : 6386 26446 26401 26414 26481 26485 26409 26472 )

// Start a process and returns if it had error (1) or not (0).
static int MapUnmapSubstExecute(std::wstring substCommand)
{
    STARTUPINFO si{};
    PROCESS_INFORMATION pi{};

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // CreateProcessW may modify the command line
    auto mutableCommandLine = std::make_unique<TCHAR[]>(substCommand.size() + 1);
    ZeroMemory(mutableCommandLine.get(), sizeof(TCHAR) * (substCommand.size() + 1));
    _tcscpy_s(mutableCommandLine.get(), substCommand.size() + 1, substCommand.c_str());

    if (!CreateProcess(nullptr, mutableCommandLine.get(), nullptr, nullptr, false, NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi))
    {
        // Release the mutex
        return 1;
    }

    printVerbose(L"{}", L"Start waiting for process Map/Unmap to complete.");
    WaitForSingleObject(pi.hProcess, INFINITE);
    printVerbose(L"{}", L"Done waiting for process Map/Unmap to complete.");

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0)
    {
        // Release the mutex
        return 1;
    }

    return 0;
}
#pragma warning( pop )

#pragma warning( push )
// warning C26461: The pointer argument 'pSubstNode' for function 'UnmapDrive' can be marked as a pointer to const (con.3).
#pragma warning( disable : 26461 )
static int UnmapDrive(PSUBST_NODE pSubstNode)
{
    int ret = 0;

    std::wstring substCommand(L"subst /D \"");
    substCommand.push_back(pSubstNode->szDriveLetter);
    substCommand.push_back(L':');
    substCommand.append(L"\"");

    ret = MapUnmapSubstExecute(substCommand.c_str());

    return ret;
}

static int MapDrive(PSUBST_NODE pSubstNode)
{
    int ret = 0;

    std::wstring substCommand(L"subst \"");
    substCommand.push_back(pSubstNode->szDriveLetter);
    substCommand.push_back(L':');
    substCommand.append(L"\" \"");
    substCommand.append(pSubstNode->szSourceDirectory, wcslen(pSubstNode->szSourceDirectory) - 1); // Skip the trailing '\\'.
    substCommand.append(L"\"");

    ret = MapUnmapSubstExecute(substCommand);

    return 0;
}
#pragma warning( pop )

// Handle the CTRL-C signal. RunInSubst.exe process should continue as long as it's child is alive to keep the
// console looking reasonable. If it were to exit, standard input control would return to the console which
// gets confusing when RunInSubst.exe's child process is still running.
//
// Only CTRL-C is handled so more a aggressive CTRL-BREAK still terminates everything immediately.
BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) noexcept
{
    switch (fdwCtrlType)
    {
        case CTRL_C_EVENT:
            return true;
        default:
            return false;
    }
}

#pragma warning( push )
// warning C26446: Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
// warning C26485: Expression 'argv': No array to pointer decay (bounds.3).
// warning C26481: Don't use pointer arithmetic. Use span instead (bounds.1).
// warning C26472: Don't use a static_cast for arithmetic conversions. Use brace initialization, gsl::narrow_cast or gsl::narrow (type.1).
#pragma warning( disable : 26446 26485 26481 26472 )
// Executes the command that was specified on in the arguments.
// Returns the exit code of the process that was started.
int ExecuteProcess(int argc, _TCHAR* argv[], int executableToRunIndex, PSUBST_NODE* pOrderedSubstList)
{
    assert(argv != nullptr);
    assert(pOrderedSubstList != nullptr);

    std::wstring commandToExecute(L"");
    bool hasSpaceInArg = false;

    if (executableToRunIndex == -1)
    {
        wprintf(L"Error: No process was specified to be executed while in subst mode.");
        return 1;
    }

    _TCHAR* procToRun = argv[executableToRunIndex];
    if (wcsstr(procToRun, L" "))
    {
        // Has space. Quote the string.
        commandToExecute.append(L"\"");
        hasSpaceInArg = true;
    }

    commandToExecute.append(procToRun);

    if (hasSpaceInArg)
    {
        commandToExecute.append(L"\"");
    }

    commandToExecute.append(L" ");

    // Skip the RUN_IN_SUBST.exe name.
    for (int i = executableToRunIndex + 1; i < argc; i++)
    {
        hasSpaceInArg = false;
        if (wcsstr(argv[i], L" "))
        {
            // Has space. Quote the string.
            commandToExecute.append(L"\"");
            hasSpaceInArg = true;
        }

        commandToExecute.append(argv[i]);

        if (hasSpaceInArg)
        {
            commandToExecute.append(L"\"");
        }

        commandToExecute.append(L" ");
    }

    STARTUPINFO si{};
    PROCESS_INFORMATION pi{};

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    auto currentDir = std::vector<wchar_t>(SUBST_SOURCE_LENGTH, 0);
    auto newCurrentDir = std::vector<wchar_t>(SUBST_SOURCE_LENGTH, 0);

    if (GetCurrentDirectory(SUBST_SOURCE_LENGTH, currentDir.data()) == 0)
    {
        wprintf(L"Error: Could not get current directory.");
        return 1;
    }

    // make sure there is a trailing '\\'.
    const size_t srcLen = wcslen(currentDir.data());

    // Convert the currentDir to lower case.
    for (size_t i = 0; i < srcLen; i++)
    {
        currentDir[i] = static_cast<TCHAR>(::tolower(currentDir[i]));
    }

    if (currentDir[srcLen - 1] != L'\\')
    {
        currentDir[srcLen] = L'\\';
    }

    size_t longestDirMatch = 0;

    // Find the start directory.
    for (int i = 0; i < NUMBER_DEFINABLE_SUBST; i++)
    {
        PSUBST_NODE pListNode = pOrderedSubstList[i];
        if (pListNode == nullptr)
        {
            continue;
        };

        // Always get the drive that closest maps to the current directory - the longest path.
        if (wcsstr(currentDir.data(), pListNode->szSourceDirectory) == currentDir.data())
        {
            // Found a match.
            const size_t len = wcslen(pListNode->szSourceDirectory);
            // Find the longest match.
            if (len > longestDirMatch)
            {
                longestDirMatch = len;
                newCurrentDir[0] = pListNode->szDriveLetter;
                newCurrentDir[1] = L':';
                newCurrentDir[2] = L'\\';
                newCurrentDir[3] = L'\0';
                wcscat_s(newCurrentDir.data(), SUBST_SOURCE_LENGTH, currentDir.data() + len);
            }
        }
    }

    // If newCurrentDirectory not set, use the currentDirectory instead.
    if (wcslen(newCurrentDir.data()) == 0)
    {
        // Not found.
        wcscpy_s(newCurrentDir.data(), SUBST_SOURCE_LENGTH, currentDir.data());
    }

    if (!CreateProcess(
        nullptr,
        (LPWSTR)commandToExecute.c_str(),
        nullptr,
        nullptr,
        false,
        NORMAL_PRIORITY_CLASS,
        nullptr,
        newCurrentDir.data(),
        &si,
        &pi))
    {
        wprintf(L"Error: Failed creating process %s.\r\n", procToRun);
        return 1;
    }

    printVerbose(L"{}", L"Start waiting for started process complete.");
    WaitForSingleObject(pi.hProcess, INFINITE);
    printVerbose(L"{}", L"Done waiting for started process complete.");

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        wprintf(L"Error: Process %s exit code could not be obtained.\r\n", procToRun);
        return 1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (int)exitCode;
}
#pragma warning( pop )

#pragma warning( push )
// warning C26485: Expression 'argv': No array to pointer decay (bounds.3).
// warning C26485: Expression 'mappedDriveLocation': No array to pointer decay (bounds.3).
// warning C26461: The pointer argument 'pSubstList' for function 'SubstDrivesAndExecute' can be marked as a pointer to const (con.3).
// warning C26481: Don't use pointer arithmetic. Use span instead (bounds.1).
// warning C6387: 'pListNode->szSourceDirectory' could be '0'.
#pragma warning( disable : 26485 26461 26481 6387 )
// Tries to substitute a drive.
// Returns 0 on success and non-zero on failure.
static int SubstDrivesAndExecute(int argc, _TCHAR* argv[], PSUBST_LIST_NODE pSubstList, PSUBST_NODE* pOrderedSubstList, int executableToRunIndex)
{
    assert(argv != nullptr);
    assert(pSubstList != nullptr);
    assert(pOrderedSubstList != nullptr);

    // Get a lock to the lock file(s) and try to subst.
    // Make sure that there were any subst executed first.

    // First, get the local locks.
    if (pSubstList != nullptr)
    {
        for (int i = 0; i < NUMBER_DEFINABLE_SUBST; )
        {
            PSUBST_NODE pListNode = pOrderedSubstList[i];
            if (pListNode == nullptr)
            {
                i++;
                continue;
            };

            // Validate that the existence of the source location and try to get
            // an exclusive write lock, using the source path.
            if (pListNode->szSourceDirectory == nullptr)
            {
                wprintf(L"Error: Invalid source location for a subst drive %C:.\r\n", pListNode->szDriveLetter);
                // Process exits. All handles closed by the OS.
                return 1;
            }

            const DWORD srcAttr = GetFileAttributes(pListNode->szSourceDirectory);
            if (srcAttr == INVALID_FILE_ATTRIBUTES)
            {
                wprintf(L"Error: Invalid source location for a subst drive %C:. The source location %s doesn't exist.\r\n",
                    pListNode->szDriveLetter,
                    pListNode->szSourceDirectory);
                // Process exits. All handles closed by the OS.
                return 1;
            }

            if ((srcAttr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                wprintf(L"Error: Invalid source location for a subst drive %C:. The source location %s is not a directory.\r\n",
                    pListNode->szDriveLetter,
                    pListNode->szSourceDirectory);
                // Process exits. All handles closed by the OS.
                return 1;
            }

            std::basic_string<TCHAR> substFileLock(pListNode->szSourceDirectory);
            substFileLock.append(SUBST_FILE_NAME);

            HANDLE substFileLockHandle = INVALID_HANDLE_VALUE;
            while (true)
            {
                substFileLockHandle = CreateFile(substFileLock.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (substFileLockHandle == INVALID_HANDLE_VALUE)
                {
                    const DWORD lastError = GetLastError();
                    if (lastError == ERROR_SHARING_VIOLATION)
                    {
                        wprintf(L"Warning: Lock file for local lock file in %s is in use by another process. Waiting for %d secs...\r\n",
                            pListNode->szSourceDirectory,
                            RUN_IN_SUBST_TIMEOUT / 1000);

                        Sleep(RUN_IN_SUBST_TIMEOUT);
                        continue;
                    }
                    else
                    {
                        wprintf(L"Error: Could not get exclusive write lock for local lock file in %s. Error: %d\r\n",
                            pListNode->szSourceDirectory,
                            (int)lastError);
                        // Process exits. All handles closed by the OS.
                        return 1;
                    }
                }

                pListNode->hLockFile = substFileLockHandle;
                break;
            }

            LogToFile(pListNode, L"Substituting drive {} for path {}.", static_cast<char>(pListNode->szDriveLetter), pListNode->szSourceDirectory);

            i++;
        }

        // Now map the drive and check to see if it worked. If not, wait for release and map again.
        for (int i = 0; i < NUMBER_DEFINABLE_SUBST;)
        {
            PSUBST_NODE pListNode = pOrderedSubstList[i];
            if (pListNode == nullptr)
            {
                i++;
                continue;
            };

            MapDrive(pListNode);

            GetMappedPaths(pOrderedSubstList);

            if (pListNode->szSourceDirectory != nullptr &&
                pListNode->szMappedPath != nullptr &&
                wcscmp(pListNode->szSourceDirectory, pListNode->szMappedPath) == 0)
            {
                i++;
                continue;
            }

            // Get a hold of the file lock.
            std::basic_string<TCHAR> substFileLock;
            substFileLock.push_back(pListNode->szDriveLetter);
            substFileLock += L':' + L'\\' + L'\0';
            substFileLock.append(SUBST_FILE_NAME);

            HANDLE substFileLockHandle = INVALID_HANDLE_VALUE;
            substFileLockHandle = CreateFile(substFileLock.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (substFileLockHandle == INVALID_HANDLE_VALUE)
            {
                const DWORD lastError = GetLastError();
                if (lastError == ERROR_SHARING_VIOLATION)
                {
                    wprintf(L"Warning: Lock file for drive %C file %s is in use by another process. Waiting for %d secs...\r\n",
                        pListNode->szDriveLetter,
                        substFileLock.c_str(),
                        RUN_IN_SUBST_TIMEOUT / 1000);

                    Sleep(RUN_IN_SUBST_TIMEOUT);
                    continue;
                }
                else
                {
                    wchar_t mappedDriveLocation[4];
                    mappedDriveLocation[0] = pListNode->szDriveLetter;
                    mappedDriveLocation[1] = L':';
                    mappedDriveLocation[2] = L'\\';
                    mappedDriveLocation[3] = L'\0';
                    const DWORD srcAttr = GetFileAttributes(mappedDriveLocation);
                    if (srcAttr == INVALID_FILE_ATTRIBUTES)
                    {
                        wprintf(L"Warning: The subst drive %C: does not seem to be using the sharing protocol. Forcing a manual release of the drive.\r\n",
                            pListNode->szDriveLetter);
                        UnmapDrive(pListNode);
                        continue;
                    }

                    if ((srcAttr & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    {
                        wprintf(L"Warning: The subst drive %C: does not seem to be using the sharing protocol - mapped location not a directory. Forcing a manual release of the drive.\r\n",
                            pListNode->szDriveLetter);
                        UnmapDrive(pListNode);
                        continue;
                    }

                    wprintf(L"Error: Could not get exclusive write lock for the substituted drive lock file %s. Error: %d\r\n",
                        substFileLock.c_str(),
                        (int)lastError);
                    // Process exits. All handles closed by the OS.
                    return 1;
                }
            }
            else if (pListNode->szMappedPath != nullptr && wcscmp(pListNode->szSourceDirectory, pListNode->szMappedPath))
            {
                // If we got the lock, but the drive is mapped to another place, unmap before trying to map again.
                UnmapDrive(pListNode);
                // We will re-CreateFile above, so close the handle.
                CloseHandle(substFileLockHandle);
                continue;
            }

            assert(false && "We should never be here!!!");
        }
    }

    const int errorCode = ExecuteProcess(argc, argv, executableToRunIndex, pOrderedSubstList);

    // Clean up whatever is needed to clean up.
    for (int i = 0; i < NUMBER_DEFINABLE_SUBST; i++)
    {
        PSUBST_NODE pListNode = pOrderedSubstList[i];
        if (pListNode == nullptr)
        {
            continue;
        };

        LogToFile(pListNode, L"Done! Unsubst drive {}: - {}.",
            static_cast<char>(pListNode->szDriveLetter), pListNode->szSourceDirectory);

        UnmapDrive(pListNode);
        assert(pListNode->hLockFile != INVALID_HANDLE_VALUE && "Invalid state. Lock file handle should not be invalid.");

        if (pListNode->hLockFile == INVALID_HANDLE_VALUE)
        {
            LogToFile(pListNode, L"Invalid state. Lock file handle should not be invalid for local file {}.", pListNode->szSourceDirectory);
            return 1;
        }

        CloseHandle(pListNode->hLockFile);
    }

    return errorCode;
}
#pragma warning( pop )

#pragma warning( push )
// warning C26401: Do not delete a raw pointer that is not an owner<T> (i.11).
// warning C26409: Avoid calling new and delete explicitly, use std::make_unique<T> instead (r.11).
#pragma warning( disable : 26401 26409 )
// main - app entry point.
int _tmain(int argc, _TCHAR* argv[])
{
    PSUBST_LIST_NODE pSubstList = nullptr;
    // Array of ordered subst target and lists.
    PSUBST_NODE* pOrderedSubstList = nullptr;
    int executableToRunIndex = -1;

    InitializeState(&pSubstList, &pOrderedSubstList, &executableToRunIndex);

    if (ParseSubstSourcesAndTargets(argc, argv, &pSubstList, pOrderedSubstList, &executableToRunIndex))
    {
        return 1;
    }

    SetConsoleCtrlHandler(CtrlHandler, true);

    const int ret = SubstDrivesAndExecute(argc, argv, pSubstList, pOrderedSubstList, executableToRunIndex);

    if (pOrderedSubstList != nullptr)
    {
        delete[] pOrderedSubstList;
    }

    if (pSubstList != nullptr)
    {
        PSUBST_LIST_NODE node = pSubstList;

        while (node != nullptr)
        {
            PSUBST_LIST_NODE temp = node;
            node = node->pNext;
            delete temp;
        }
    }

    return ret;
}
#pragma warning( pop )
