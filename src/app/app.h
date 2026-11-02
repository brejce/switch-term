#pragma once
#include <string>

#include "platform/display_backend.h"

namespace app {

// 新架构主循环：新 UI（ui/screen）+ TerminalParser + PTY + 内置拼音 IME。
// 只依赖接口，不认具体终端实现。
int run(disp::DisplayBackend* bd, const std::string& resDir, const std::string& outPath,
         const std::string& runCmd);

}  // namespace app
