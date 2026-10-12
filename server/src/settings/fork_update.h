#pragma once
#include <filesystem>
#include <string>

namespace fork_update
{
struct DownloadResult
{
    std::filesystem::path installer;
    std::string message;
};
bool ValidRequest(const std::string &tag, const std::string &name, const std::string &sha256);
bool AllowedUrl(const std::string &url);
DownloadResult Download(const std::string &tag, const std::string &name, const std::string &sha256);
} // namespace fork_update
