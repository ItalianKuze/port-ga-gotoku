/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Windows-only state the HLE runtime needs.
 *
 * Everything else -- the guest memory backend (direct pool, flexible memory, reservations,
 * protection, the PS4 address range reservation), the per-service import resolvers, guest errno
 * and the GPU memory hooks -- is the same code as the Linux build; it lives in runtime_memory.c
 * and friends behind small #ifdef _WIN32 branches. This file holds only what has no portable
 * equivalent:
 *
 *   runtime_fault_recover  VEH recovery point; a CONTEXT on Windows, a sigjmp_buf elsewhere
 *
 * The guest-memory primitives themselves are the ones from Supermedo's Windows port: the PS4 user
 * range is reserved once with a VirtualAlloc2 placeholder and carved with MapViewOfFile3, so a
 * physical range maps to a fixed guest address the game can store in its 40-bit pointers.
 */
#define _WIN32_WINNT 0x0A00 /* VirtualAlloc2 / MapViewOfFile3 / CreateFileMapping2 (Win10 2004+) */
#include <windows.h>

#include "runtime.h"

/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery point
 * (probe.c's VEH): a fault on that thread restores the stored CONTEXT instead of terminating. */
__thread CONTEXT *runtime_fault_recover;