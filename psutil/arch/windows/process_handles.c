/*
 * Copyright (c) 2009, Giampaolo Rodola', Jeff Tang. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// XXX: this is very old code and very likely broken. Errors are checked
// but instead of an exception we get an empty list + debug message.
// Author: .

#include <windows.h>
#include <Python.h>

#include "../../_psutil_common.h"
#include "process_utils.h"


CRITICAL_SECTION g_cs;
BOOL g_initialized = FALSE;
NTSTATUS g_status;
HANDLE g_hFile = NULL;
HANDLE g_hEvtStart = NULL;
HANDLE g_hEvtFinish = NULL;
HANDLE g_hThread = NULL;
PUNICODE_STRING g_pNameBuffer = NULL;
ULONG g_dwSize = 0;
ULONG g_dwLength = 0;

#define NTQO_TIMEOUT 100
#define MALLOC_ZERO(x) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (x))


static int
psutil_get_open_files_init(BOOL threaded) {
    // Create events for signalling work between threads
    if (threaded == TRUE) {
        g_hEvtStart = CreateEvent(NULL, FALSE, FALSE, NULL);
        g_hEvtFinish = CreateEvent(NULL, FALSE, FALSE, NULL);
        if ((g_hEvtStart == NULL) || (g_hEvtFinish == NULL)) {
            psutil_debug("CreateEvent failed");
            return 1;
        }
        InitializeCriticalSection(&g_cs);
    }

    g_initialized = TRUE;
    return 0;
}


static DWORD WINAPI
psutil_wait_thread(LPVOID lpvParam) {
    // Loop infinitely waiting for work
    while (TRUE) {
        if (WaitForSingleObject(g_hEvtStart, INFINITE) == WAIT_FAILED)
            psutil_debug("WaitForSingleObject failed");
        g_status = NtQueryObject(
            g_hFile,
            ObjectNameInformation,
            g_pNameBuffer,
            g_dwSize,
            &g_dwLength);
        if (SetEvent(g_hEvtFinish) == 0)
            psutil_debug("SetEvent failed");
    }
}


static DWORD
psutil_create_thread() {
    DWORD dwWait = 0;

    if (g_hThread == NULL)
        g_hThread = CreateThread(
            NULL,
            0,
            psutil_wait_thread,
            NULL,
            0,
            NULL);
    if (g_hThread == NULL) {
        psutil_debug("CreateThread failed");
        return GetLastError();
    }

    // Signal the worker thread to start
    SetEvent(g_hEvtStart);

    // Wait for the worker thread to finish
    dwWait = WaitForSingleObject(g_hEvtFinish, NTQO_TIMEOUT);
    if (dwWait == WAIT_FAILED)
        psutil_debug("WaitForSingleObject failed");

    // If the thread hangs, kill it and cleanup
    if (dwWait == WAIT_TIMEOUT) {
        if (SuspendThread(g_hThread) == -1)
            psutil_debug("SuspendThread failed");
        if (TerminateThread(g_hThread, 1) == 0)
            psutil_debug("TerminateThread failed");
        if (WaitForSingleObject(g_hThread, INFINITE))
            psutil_debug("WaitForSingleObject failed");
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }

    return dwWait;
}


PyObject *
psutil_get_open_files(DWORD dwPid, HANDLE hProcess) {
    NTSTATUS                            status;
    PSYSTEM_HANDLE_INFORMATION_EX       pHandleInfo = NULL;
    DWORD                               dwInfoSize = 0x10000;
    DWORD                               dwRet = 0;
    PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX  hHandle = NULL;
    DWORD                               i = 0;
    BOOLEAN                             error = FALSE;
    DWORD                               dwWait = 0;
    PyObject*                           py_path = NULL;
    PyObject*                           py_retlist = PyList_New(0);

    if (!py_retlist)
        return NULL;
    if (g_initialized == FALSE)
        if (psutil_get_open_files_init(TRUE) == 1)
            return NULL;

    // Due to the use of global variables, ensure only 1 call
    // to psutil_get_open_files() is running
    EnterCriticalSection(&g_cs);

    if (g_hEvtStart == NULL || g_hEvtFinish == NULL) {
        PyErr_SetFromWindowsErr(0);
        error = TRUE;
        goto cleanup;
    }

    // Py_BuildValue raises an exception if NULL is returned
    if (py_retlist == NULL) {
        error = TRUE;
        goto cleanup;
    }

    do {
        if (pHandleInfo != NULL) {
            FREE(pHandleInfo);
            pHandleInfo = NULL;
        }

        // NtQuerySystemInformation won't give us the correct buffer size,
        // so we guess by doubling the buffer size.
        dwInfoSize *= 2;
        pHandleInfo = MALLOC_ZERO(dwInfoSize);

        if (pHandleInfo == NULL) {
            psutil_debug("malloc failed");
            PyErr_NoMemory();
            error = TRUE;
            goto cleanup;
        }
    } while ((status = NtQuerySystemInformation(
                            SystemExtendedHandleInformation,
                            pHandleInfo,
                            dwInfoSize,
                            &dwRet)) == STATUS_INFO_LENGTH_MISMATCH);

    // NtQuerySystemInformation stopped giving us STATUS_INFO_LENGTH_MISMATCH
    if (! NT_SUCCESS(status)) {
        psutil_debug("NtQuerySystemInformation failed");
        psutil_SetFromNTStatusErr(
            status, "NtQuerySystemInformation(SystemExtendedHandleInformation)");
        error = TRUE;
        goto cleanup;
    }

    for (i = 0; i < pHandleInfo->NumberOfHandles; i++) {
        hHandle = &pHandleInfo->Handles[i];

        // Check if this hHandle belongs to the PID the user specified.
        if ((ULONG_PTR)hHandle->UniqueProcessId != dwPid)
            goto loop_cleanup;

        if (!DuplicateHandle(hProcess,
                             (HANDLE)hHandle->HandleValue,
                             GetCurrentProcess(),
                             &g_hFile,
                             0,
                             TRUE,
                             DUPLICATE_SAME_ACCESS))
        {
            psutil_debug("DuplicateHandle failed");
            goto loop_cleanup;
        }

        // Guess buffer size is MAX_PATH + 1
        g_dwLength = (MAX_PATH+1) * sizeof(WCHAR);

        do {
            // Release any previously allocated buffer
            if (g_pNameBuffer != NULL) {
                FREE(g_pNameBuffer);
                g_pNameBuffer = NULL;
                g_dwSize = 0;
            }

            // NtQueryObject puts the required buffer size in g_dwLength
            // WinXP edge case puts g_dwLength == 0, just skip this handle
            if (g_dwLength == 0)
                goto loop_cleanup;

            g_dwSize = g_dwLength;
            if (g_dwSize > 0) {
                g_pNameBuffer = MALLOC_ZERO(g_dwSize);

                if (g_pNameBuffer == NULL)
                    goto loop_cleanup;
            }

            dwWait = psutil_create_thread();

            // If the call does not return, skip this handle
            if (dwWait != WAIT_OBJECT_0)
                goto loop_cleanup;

        } while (g_status == STATUS_INFO_LENGTH_MISMATCH);

        // NtQueryObject stopped returning STATUS_INFO_LENGTH_MISMATCH
        if (!NT_SUCCESS(g_status)) {
            psutil_debug("NtQueryObject failed");
            goto loop_cleanup;
        }

        // Convert to PyUnicode and append it to the return list
        if (g_pNameBuffer->Length > 0) {
            py_path = PyUnicode_FromWideChar(g_pNameBuffer->Buffer,
                                             g_pNameBuffer->Length / 2);
            if (py_path == NULL) {
                psutil_debug("PyUnicode_FromWideChar failed");
                error = TRUE;
                goto loop_cleanup;
            }

            if (PyList_Append(py_retlist, py_path)) {
                psutil_debug("PyList_Append failed");
                error = TRUE;
                goto loop_cleanup;
            }
        }

loop_cleanup:
    Py_XDECREF(py_path);
    py_path = NULL;
    if (g_pNameBuffer != NULL)
        FREE(g_pNameBuffer);
    g_pNameBuffer = NULL;
    g_dwSize = 0;
    g_dwLength = 0;
    if (g_hFile != NULL)
        CloseHandle(g_hFile);
    g_hFile = NULL;
}

cleanup:
    if (g_pNameBuffer != NULL)
        FREE(g_pNameBuffer);
    g_pNameBuffer = NULL;
    g_dwSize = 0;
    g_dwLength = 0;

    if (g_hFile != NULL)
        CloseHandle(g_hFile);
    g_hFile = NULL;

    if (pHandleInfo != NULL)
        FREE(pHandleInfo);
    pHandleInfo = NULL;

    if (error) {
        Py_XDECREF(py_retlist);
        py_retlist = NULL;
    }

    LeaveCriticalSection(&g_cs);
    return py_retlist;
}
