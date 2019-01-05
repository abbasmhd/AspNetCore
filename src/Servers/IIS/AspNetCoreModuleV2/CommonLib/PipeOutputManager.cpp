// Copyright (c) .NET Foundation. All rights reserved.
// Licensed under the MIT License. See License.txt in the project root for license information.

#include "PipeOutputManager.h"

#include "stdafx.h"
#include "Exceptions.h"
#include "SRWExclusiveLock.h"
#include "StdWrapper.h"
#include "ntassert.h"
#include "StringHelpers.h"

#define LOG_IF_DUPFAIL(err) do { if (err == -1) { LOG_IF_FAILED(HRESULT_FROM_WIN32(_doserrno)); } } while (0, 0);
#define LOG_IF_ERRNO(err) do { if (err != 0) { LOG_IF_FAILED(HRESULT_FROM_WIN32(_doserrno)); } } while (0, 0);


PipeOutputManager::PipeOutputManager(RedirectionOutput& output) :
    m_output(output),
    m_hErrReadPipe(INVALID_HANDLE_VALUE),
    m_hErrWritePipe(INVALID_HANDLE_VALUE),
    m_hErrThread(nullptr),
    m_disposed(false)
{
    TryStartRedirection();
}

PipeOutputManager::~PipeOutputManager() noexcept(false)
{
    TryStopRedirection();
}

// Start redirecting stdout and stderr into a pipe
// Continuously read the pipe on a background thread
// until Stop is called.
void PipeOutputManager::Start()
{
    SECURITY_ATTRIBUTES     saAttr = { 0 };
    HANDLE                  hStdErrReadPipe;
    HANDLE                  hStdErrWritePipe;

    // To make Console.* functions work, allocate a console
    // in the current process.
    if (!AllocConsole())
    {
        // ERROR_ACCESS_DENIED means there is a console already present.
        if (GetLastError() != ERROR_ACCESS_DENIED)
        {
            THROW_LAST_ERROR();
        }
    }

    THROW_LAST_ERROR_IF(!CreatePipe(&hStdErrReadPipe, &hStdErrWritePipe, &saAttr, 0 /*nSize*/));

    m_hErrReadPipe = hStdErrReadPipe;
    m_hErrWritePipe = hStdErrWritePipe;

    stdoutWrapper = std::make_unique<StdWrapper>(stdout, STD_OUTPUT_HANDLE, hStdErrWritePipe, true);
    stderrWrapper = std::make_unique<StdWrapper>(stderr, STD_ERROR_HANDLE, hStdErrWritePipe, true);

    LOG_IF_FAILED(stdoutWrapper->StartRedirection());
    LOG_IF_FAILED(stderrWrapper->StartRedirection());

    // Read the stderr handle on a separate thread until we get 30Kb.
    m_hErrThread = CreateThread(
        nullptr,       // default security attributes
        0,          // default stack size
        reinterpret_cast<LPTHREAD_START_ROUTINE>(ReadStdErrHandle),
        this,       // thread function arguments
        0,          // default creation flags
        nullptr);      // receive thread identifier

    THROW_LAST_ERROR_IF_NULL(m_hErrThread);
}

// Stop redirecting stdout and stderr into a pipe
// This closes the background thread reading from the pipe
// and prints any output that was captured in the pipe.
// If more than 30Kb was written to the pipe, that output will
// be thrown away.
void PipeOutputManager::Stop()
{
    DWORD    dwThreadStatus = 0;

    if (m_disposed)
    {
        return;
    }

    SRWExclusiveLock lock(m_srwLock);

    if (m_disposed)
    {
        return;
    }

    m_disposed = true;

    // Both pipe wrappers duplicate the pipe writer handle
    // meaning we are fine to close the handle too.
    if (m_hErrWritePipe != INVALID_HANDLE_VALUE)
    {
        // Flush the pipe writer before closing to capture all output
        THROW_LAST_ERROR_IF(!FlushFileBuffers(m_hErrWritePipe));
        CloseHandle(m_hErrWritePipe);
        m_hErrWritePipe = INVALID_HANDLE_VALUE;
    }

    // Tell each pipe wrapper to stop redirecting output and restore the original values
    if (stdoutWrapper != nullptr)
    {
        LOG_IF_FAILED(stdoutWrapper->StopRedirection());
    }

    if (stderrWrapper != nullptr)
    {
        LOG_IF_FAILED(stderrWrapper->StopRedirection());
    }

    // Forces ReadFile to cancel, causing the read loop to complete.
    // Don't check return value as IO may or may not be completed already.
    if (m_hErrThread != nullptr)
    {
        LOG_INFO(L"Cancelling standard stream pipe reader");
        CancelSynchronousIo(m_hErrThread);
    }

    // GetExitCodeThread returns 0 on failure; thread status code is invalid.
    if (m_hErrThread != nullptr &&
        !LOG_LAST_ERROR_IF(!GetExitCodeThread(m_hErrThread, &dwThreadStatus)) &&
        dwThreadStatus == STILL_ACTIVE)
    {
        // Wait for graceful shutdown, i.e., the exit of the background thread or timeout
        if (WaitForSingleObject(m_hErrThread, PIPE_OUTPUT_THREAD_TIMEOUT) != WAIT_OBJECT_0)
        {
            // If the thread is still running, we need kill it first before exit to avoid AV
            if (!LOG_LAST_ERROR_IF(GetExitCodeThread(m_hErrThread, &dwThreadStatus) == 0) &&
                dwThreadStatus == STILL_ACTIVE)
            {
                LOG_WARN(L"Thread reading stdout/err hit timeout, forcibly closing thread.");
                TerminateThread(m_hErrThread, STATUS_CONTROL_C_EXIT);
            }
        }
    }

    if (m_hErrThread != nullptr)
    {
        CloseHandle(m_hErrThread);
        m_hErrThread = nullptr;
    }

    if (m_hErrReadPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hErrReadPipe);
        m_hErrReadPipe = INVALID_HANDLE_VALUE;
    }
}


void
PipeOutputManager::ReadStdErrHandle(
    LPVOID pContext
)
{
    auto pLoggingProvider = static_cast<PipeOutputManager*>(pContext);
    DBG_ASSERT(pLoggingProvider != NULL);
    pLoggingProvider->ReadStdErrHandleInternal();
}

void
PipeOutputManager::ReadStdErrHandleInternal()
{
    std::string tempBuffer;
    tempBuffer.resize(PIPE_READ_SIZE);

    // If ReadFile ever returns false, exit the thread
    DWORD dwNumBytesRead = 0;
    while (true)
    {
        if (ReadFile(m_hErrReadPipe,
            tempBuffer.data(),
            PIPE_READ_SIZE,
            &dwNumBytesRead,
            nullptr))
        {
            m_output.Append(to_wide_string(tempBuffer.substr(0, dwNumBytesRead), GetConsoleOutputCP()));
        }
        else
        {
            return;
        }
    }
}
