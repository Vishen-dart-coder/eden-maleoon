// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/crash_guard.h"

namespace Common::CrashGuard {

namespace {
thread_local sigjmp_buf* g_env = nullptr;
}

void Enter(sigjmp_buf* env) {
    g_env = env;
}

void Leave() {
    g_env = nullptr;
}

void TryRecover() {
    sigjmp_buf* const env = g_env;
    if (env == nullptr) {
        return;
    }
    g_env = nullptr;
    siglongjmp(*env, 1);
}

} // namespace Common::CrashGuard
