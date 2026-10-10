/* bbport: Windows launcher.
 *
 * The HLE runtime lives in bbport_runtime.dll because libbbgpu.dll has to import the runtime_*
 * API from it (a PE DLL cannot carry unresolved externals), and the runtime in turn calls the
 * bbgpu_* entry points. Keeping the loader in the DLL rather than in the executable also means
 * the runtime_* symbols are exported by a module the GPU DLL can name in its import table,
 * which is what the ELF build gets for free from -rdynamic.
 *
 * This executable only locates that DLL next to itself and forwards the command line. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*RuntimeMain)(int argc, char **argv);

int main(int argc, char **argv) {
    /* The guest keeps its thread pointer in a TEB TLS slot, addressed as gs:[0x1480+8*slot].
     * TEB.TlsSlots only covers slots 0..63, and this DLL's imports (SDL3, Vulkan, FFmpeg) claim
     * most of them while it loads, so the slot is reserved here: bb-probe.exe links kernel32 only
     * and runs before any of those are mapped. The reserved slot is handed to the runtime, which
     * must not call TlsAlloc() itself. */
    DWORD tcb_slot = TlsAlloc();
    if (tcb_slot == TLS_OUT_OF_INDEXES || tcb_slot >= 64) {
        fprintf(stderr, "bb-probe: cannot reserve a TEB TLS slot for the guest TCB (%lu)\n",
                (unsigned long)tcb_slot);
        return 1;
    }

    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        fprintf(stderr, "bb-probe: cannot locate own path\n");
        return 1;
    }
    char *slash = strrchr(path, '\\');
    if (slash) slash[1] = '\0';
    else path[0] = '\0';
    strncat(path, "bbport_runtime.dll", MAX_PATH - strlen(path) - 1);

    /* ANSI so the argv the guest sees keeps the same encoding as the Windows build. */
    HMODULE dll = LoadLibraryA(path);
    if (!dll) {
        fprintf(stderr, "bb-probe: cannot load %s (error %lu)\n", path, (unsigned long)GetLastError());
        return 1;
    }
    RuntimeMain entry = (RuntimeMain)(void*)GetProcAddress(dll, "bbport_main");
    if (!entry) {
        fprintf(stderr, "bb-probe: %s has no bbport_main\n", path);
        return 1;
    }
    /* Hand the reserved TLS slot over before anything in the runtime allocates one. */
    typedef void (*AdoptSlot)(unsigned);
    AdoptSlot adopt = (AdoptSlot)(void*)GetProcAddress(dll, "runtime_thread_adopt_slot");
    if (!adopt) {
        fprintf(stderr, "bb-probe: %s has no runtime_thread_adopt_slot\n", path);
        return 1;
    }
    adopt((unsigned)tcb_slot);
    /* Forward argv unchanged: the loader expects argv[0] to be the program name, argv[1] the
     * boot image and the options from argv[2], exactly as the Linux build does. */
    return entry(argc, argv);
}