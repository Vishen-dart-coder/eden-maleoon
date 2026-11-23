// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <csetjmp>

namespace Common::CrashGuard {

/// Marks the calling thread as inside a region whose crashes can be recovered from.
void Enter(sigjmp_buf* env);
void Leave();

/// Called from a SIGSEGV/SIGBUS handler. Jumps back into the guarded region if the
/// crashing thread is inside one; returns normally otherwise.
void TryRecover();

} // namespace Common::CrashGuard
