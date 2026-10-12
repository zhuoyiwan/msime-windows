#include <exception>
#include <fmt/core.h>
#include "tests/includes/test_framework.h"
#include <string_view>

namespace test
{
std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char *name, std::function<void()> fn)
{
    registry().push_back({name, std::move(fn)});
}
} // namespace test

int main(int argc, char **argv)
{
    const std::string_view filter = argc == 3 && std::string_view(argv[1]) == "--filter" ? argv[2] : "";
    if (argc != 1 && filter.empty())
        return 2;
    int failures = 0;
    size_t executed = 0;
    for (const auto &test_case : test::registry())
    {
        if (!filter.empty() && test_case.name.find(filter) == std::string::npos)
            continue;
        ++executed;
        try
        {
            test_case.fn();
            fmt::print("[PASS] {}\n", test_case.name);
        }
        catch (const std::exception &ex)
        {
            ++failures;
            fmt::print(stderr, "[FAIL] {}: {}\n", test_case.name, ex.what());
        }
        catch (...)
        {
            ++failures;
            fmt::print(stderr, "[FAIL] {}: unknown exception\n", test_case.name);
        }
    }

    if (failures > 0)
    {
        fmt::print(stderr, "{} test(s) failed\n", failures);
        return 1;
    }

    fmt::print("{} test(s) passed\n", executed);
    return executed ? 0 : 2;
}
