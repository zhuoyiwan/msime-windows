#include "fork_update.h"
#include <Windows.h>
#include <bcrypt.h>
#include <curl/curl.h>
#include <array>
#include <chrono>
#include <fstream>
#include <regex>
#include <vector>

namespace fork_update
{
namespace
{
constexpr size_t kMaxBytes = 256ULL * 1024 * 1024;
struct Transfer
{
    std::ofstream file;
    BCRYPT_HASH_HANDLE hash = nullptr;
    size_t bytes = 0;
    bool redirect = false;
    std::string location;
};
size_t Header(char *data, size_t size, size_t count, void *user)
{
    auto &state = *static_cast<Transfer *>(user);
    const std::string line(data, size * count);
    if (line.rfind("HTTP/", 0) == 0)
    {
        const auto space = line.find(' ');
        state.redirect = space != std::string::npos && line.size() > space + 1 && line[space + 1] == '3';
        state.location.clear();
    }
    std::string lower = line;
    for (char &ch : lower)
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
    if (lower.rfind("location:", 0) == 0)
    {
        const auto start = line.find_first_not_of(" \t", 9);
        const auto end = line.find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end >= start)
            state.location = line.substr(start, end - start + 1);
    }
    return size * count;
}
size_t Write(char *data, size_t size, size_t count, void *user)
{
    auto &state = *static_cast<Transfer *>(user);
    const size_t bytes = size * count;
    if (state.redirect)
        return bytes;
    if (bytes > kMaxBytes - state.bytes || bytes > ULONG_MAX)
        return 0;
    if (BCryptHashData(state.hash, reinterpret_cast<PUCHAR>(data), static_cast<ULONG>(bytes), 0) != 0)
        return 0;
    state.file.write(data, static_cast<std::streamsize>(bytes));
    if (!state.file)
        return 0;
    state.bytes += bytes;
    return bytes;
}
std::string Hex(const unsigned char *bytes, size_t length)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    for (size_t index = 0; index < length; ++index)
    {
        value += digits[bytes[index] >> 4];
        value += digits[bytes[index] & 15];
    }
    return value;
}
} // namespace

bool ValidRequest(const std::string &tag, const std::string &name, const std::string &sha256)
{
    static const std::regex tag_pattern("zhuoyiwan-[A-Za-z0-9._-]{1,100}");
    static const std::regex name_pattern(
        "MetasequoiaIME_Setup_v[0-9]+\\.[0-9]+\\.[0-9]+-zhuoyiwan-local-[0-9]{8}-r[0-9]+(-unsigned)?\\.exe");
    static const std::regex digest_pattern("[0-9a-f]{64}");
    return std::regex_match(tag, tag_pattern) && std::regex_match(name, name_pattern) &&
           std::regex_match(sha256, digest_pattern);
}
bool AllowedUrl(const std::string &url)
{
    // 每次跳转都限制 HTTPS 与精确主机，不接受用户信息、非标准端口或近似域名。
    static const std::regex pattern(
        "https://(github\\.com|release-assets\\.githubusercontent\\.com|objects\\.githubusercontent\\.com)/[^\\r\\n]*");
    return std::regex_match(url, pattern);
}
DownloadResult Download(const std::string &tag, const std::string &name, const std::string &sha256)
{
    if (!ValidRequest(tag, name, sha256))
        return {{}, "更新信息不合法"};
    static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready)
        return {{}, "无法初始化更新下载"};
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) !=
        0)
        return {{}, "无法创建更新缓存"};
    std::error_code error;
    auto root = std::filesystem::temp_directory_path(error);
    if (error)
        return {{}, "无法读取临时目录"};
    auto directory = root / ("msime-update-" + Hex(random.data(), random.size()));
    if (!std::filesystem::create_directory(directory, error) || error)
        return {{}, "无法创建更新缓存"};
    auto target = directory / name;
    const auto fail = [&](const char *message) -> DownloadResult {
        std::filesystem::remove(target, error);
        std::filesystem::remove(directory, error);
        return {{}, message};
    };
    std::string url = "https://github.com/zhuoyiwan/msime-windows/releases/download/" + tag + "/" + name;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return fail("无法校验更新安装包");
    bool ok = false;
    for (int hop = 0; hop < 6 && AllowedUrl(url); ++hop)
    {
        Transfer state;
        state.file.open(target, std::ios::binary | std::ios::trunc);
        if (!state.file || BCryptCreateHash(algorithm, &state.hash, nullptr, 0, nullptr, 0, 0) != 0)
            break;
        CURL *curl = curl_easy_init();
        if (!curl)
        {
            BCryptDestroyHash(state.hash);
            break;
        }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "MetasequoiaIME-zhuoyiwan-updater");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, Header);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        const auto result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        state.file.close();
        unsigned char digest[32]{};
        const bool digest_ok = BCryptFinishHash(state.hash, digest, sizeof(digest), 0) == 0;
        BCryptDestroyHash(state.hash);
        if (result != CURLE_OK)
            break;
        if (status == 200 && state.bytes > 0 && digest_ok && Hex(digest, sizeof(digest)) == sha256)
        {
            ok = true;
            break;
        }
        if (!state.redirect || state.location.empty())
            break;
        url = state.location;
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? DownloadResult{target, "安装包已校验，正在打开安装向导"}
              : fail("下载或校验失败，未运行安装包；请重试或前往 GitHub 下载");
}
} // namespace fork_update
