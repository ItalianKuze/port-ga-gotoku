// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: single entry point for <windows.h> in the port. The Windows headers still define the
// Windows 3.x keyword macros near/far and, in winnt.h, MemoryBarrier as the x86 intrinsic
// _mm_mfence -- all of which collide with Vulkan's vk::MemoryBarrier and with ordinary C++
// identifiers in shadPS4 and the shim. Include this instead of <windows.h> so the cleanup
// happens exactly once, after the header is fully parsed.
#pragma once

#include <windows.h>

// Legacy Windows 3.x keywords (minwindef.h).
#undef near
#undef far
#undef NEAR
#undef FAR

// Vulkan's vk::MemoryBarrier and the HLSL intrinsics share this name.
#undef MemoryBarrier

// winbase.h file/window helpers that shadow common C++ names.
#undef CreateWindow
#undef CreateFile
#undef DeleteFile
#undef MoveFile
#undef CopyFile
#undef GetTempPath
#undef GetCurrentDirectory
#undef GetMessage
#undef SendMessage

// objbase.h / winerror.h.
#undef interface
#undef ERROR