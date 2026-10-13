#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

constexpr DWORD kPathCapacity = 32768;

const wchar_t* argumentTail(const wchar_t* commandLine) {
    const wchar_t* cursor = commandLine;
    while (*cursor == L' ' || *cursor == L'\t') {
        ++cursor;
    }

    if (*cursor == L'"') {
        ++cursor;
        while (*cursor && *cursor != L'"') {
            ++cursor;
        }
        if (*cursor == L'"') {
            ++cursor;
        }
    } else {
        while (*cursor && *cursor != L' ' && *cursor != L'\t') {
            ++cursor;
        }
    }
    while (*cursor == L' ' || *cursor == L'\t') {
        ++cursor;
    }
    return cursor;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t appPath[kPathCapacity]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, appPath, kPathCapacity);
    if (pathLength == 0 || pathLength >= kPathCapacity) {
        return static_cast<int>(GetLastError());
    }

    wchar_t* separator = nullptr;
    for (wchar_t* cursor = appPath; *cursor; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            separator = cursor;
        }
    }
    if (separator == nullptr) {
        return ERROR_BAD_PATHNAME;
    }
    *(separator + 1) = L'\0';

    constexpr wchar_t kApplicationRelativePath[] = L"dependencies\\CompressoApp.exe";
    const DWORD directoryLength = static_cast<DWORD>(separator + 1 - appPath);
    constexpr DWORD applicationPathLength =
        static_cast<DWORD>(sizeof(kApplicationRelativePath) / sizeof(wchar_t) - 1);
    if (directoryLength + applicationPathLength >= kPathCapacity) {
        return ERROR_FILENAME_EXCED_RANGE;
    }
    for (DWORD index = 0; index < applicationPathLength; ++index) {
        appPath[directoryLength + index] = kApplicationRelativePath[index];
    }
    appPath[directoryLength + applicationPathLength] = L'\0';

    if (GetFileAttributesW(appPath) == INVALID_FILE_ATTRIBUTES) {
        return ERROR_FILE_NOT_FOUND;
    }

    wchar_t processCommandLine[kPathCapacity]{};
    DWORD commandLength = 0;
    processCommandLine[commandLength++] = L'"';
    for (DWORD index = 0; index < directoryLength + applicationPathLength; ++index) {
        processCommandLine[commandLength++] = appPath[index];
    }
    processCommandLine[commandLength++] = L'"';

    const wchar_t* tail = argumentTail(GetCommandLineW());
    const DWORD tailLength = lstrlenW(tail);
    if (commandLength + tailLength + 1 >= kPathCapacity) {
        return ERROR_BUFFER_OVERFLOW;
    }
    if (tailLength > 0) {
        processCommandLine[commandLength++] = L' ';
        for (DWORD index = 0; index < tailLength; ++index) {
            processCommandLine[commandLength++] = tail[index];
        }
    }
    processCommandLine[commandLength] = L'\0';

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(appPath, processCommandLine, nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                        &startup, &process)) {
        return static_cast<int>(GetLastError());
    }

    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
