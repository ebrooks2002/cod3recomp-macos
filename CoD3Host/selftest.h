#pragma once

#include <string>

namespace SelfTest
{
    // Runs the requested automated self-test suite ("vm", "runtime", "render",
    // "audio", or "all") and returns 0 on pass, non-zero on failure.
    int Run(const std::string& suite);
}
