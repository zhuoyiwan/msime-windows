#include "settings/fork_update.h"
#include "tests/includes/test_framework.h"

TEST_CASE(fork_update_rejects_foreign_assets_and_path_traversal)
{
    const std::string tag = "zhuoyiwan-20261013-r3";
    const std::string name = "MetasequoiaIME_Setup_v0.7.1-zhuoyiwan-local-20261013-r9-unsigned.exe";
    const std::string digest(64, 'a');
    REQUIRE(fork_update::ValidRequest(tag, name, digest));
    REQUIRE(!fork_update::ValidRequest("../escape", name, digest));
    REQUIRE(!fork_update::ValidRequest(tag, "../" + name, digest));
    REQUIRE(!fork_update::ValidRequest(tag, "MetasequoiaIME_Setup_v0.7.1.exe", digest));
    REQUIRE(!fork_update::ValidRequest(tag, name, "abc"));
}
TEST_CASE(fork_update_redirects_are_https_and_exact_hosts)
{
    REQUIRE(fork_update::AllowedUrl("https://github.com/zhuoyiwan/msime-windows/releases/download/x/y"));
    REQUIRE(fork_update::AllowedUrl("https://release-assets.githubusercontent.com/asset?token=synthetic"));
    REQUIRE(!fork_update::AllowedUrl("http://github.com/x"));
    REQUIRE(!fork_update::AllowedUrl("https://github.com.evil.example/x"));
    REQUIRE(!fork_update::AllowedUrl("https://user@github.com/x"));
    REQUIRE(!fork_update::AllowedUrl("https://github.com:8443/x"));
    REQUIRE(!fork_update::AllowedUrl("https://evil.example/x"));
}
