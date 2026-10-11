// The standalone Windows integration target needs only this path provider. Production and the
// full test executable link ime_paths.cpp; here the fixture must supply an isolated directory.
#include "utils/common_utils.h"

std::wstring CommonUtils::get_ime_data_path_w()
{
    const DWORD size = GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", nullptr, 0);
    if (!size)
        return {};
    std::wstring path(size, L'\0');
    GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", path.data(), size);
    path.resize(size - 1);
    return path;
}
