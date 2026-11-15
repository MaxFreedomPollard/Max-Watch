#pragma once

#include <string>

namespace maxwatch {

// Runs the complete 14-stage automated Windows verification suite in `scratchDir`.
// Returns 0 on 100% pass, or non-zero exit code with detailed diagnostics on failure.
int runAutomatedSelfTest(const std::wstring& scratchDir);

} // namespace maxwatch
