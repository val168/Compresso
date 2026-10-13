#include "compresso/filesystem_compression.h"

#include <bit>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

compresso::WofBackingState compresso::inspectWofBackingState(
    const std::filesystem::path& path)
{
#ifdef _WIN32
    constexpr ULONG wofProviderWim = 1;
    constexpr ULONG wofProviderFile = 2;
    constexpr ULONG fileProviderCompressionLzx = 1;
    struct WofCompressionInfo
    {
        ULONG algorithm;
        ULONG flags;
    };
    using WofIsExternalFileFunction = HRESULT(WINAPI*)(LPCWSTR, PBOOL, PULONG, PVOID, PULONG);

    static const HMODULE wofModule = LoadLibraryExW(
        L"wofutil.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (wofModule == nullptr) return WofBackingState::unavailable;

    const FARPROC functionAddress = GetProcAddress(wofModule, "WofIsExternalFile");
    static_assert(sizeof(functionAddress) == sizeof(WofIsExternalFileFunction));
    const auto inspect = std::bit_cast<WofIsExternalFileFunction>(functionAddress);
    if (inspect == nullptr) return WofBackingState::unavailable;

    BOOL external = FALSE;
    ULONG provider = 0;
    WofCompressionInfo info{};
    ULONG infoLength = sizeof(info);
    const HRESULT result = inspect(path.c_str(), &external, &provider, &info, &infoLength);
    if (FAILED(result)) return WofBackingState::unavailable;
    if (external == FALSE) return WofBackingState::physical_file;
    if (provider != wofProviderFile && provider != wofProviderWim)
        return WofBackingState::other_external_provider;
    if (provider == wofProviderWim) return WofBackingState::other_external_provider;
    if (infoLength < sizeof(info)) return WofBackingState::unavailable;
    return info.algorithm == fileProviderCompressionLzx
        ? WofBackingState::lzx_compressed
        : WofBackingState::other_external_provider;
#else
    (void)path;
    return WofBackingState::unavailable;
#endif
}

compresso::FileCompressionState compresso::inspectFileCompressionState(
    const std::filesystem::path& path, bool ntfsCompressedAttribute,
    bool allocationSuggestsCompression)
{
    // WOF-backed LZX files are not guaranteed to advertise FILE_ATTRIBUTE_COMPRESSED
    // or occupy fewer bytes than their logical size. Always query WOF metadata before
    // using those hints to classify a file as uncompressed.
    switch (inspectWofBackingState(path)) {
    case WofBackingState::lzx_compressed:
        return FileCompressionState::lzx_compressed;
    case WofBackingState::other_external_provider:
        return FileCompressionState::other;
    case WofBackingState::physical_file:
        return ntfsCompressedAttribute ? FileCompressionState::other
            : allocationSuggestsCompression ? FileCompressionState::unknown
                                            : FileCompressionState::uncompressed;
    case WofBackingState::unavailable:
        return FileCompressionState::unknown;
    }
    return FileCompressionState::unknown;
}
