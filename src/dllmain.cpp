#include "overlay.hpp"
#include "external_overlay.hpp"
#include "voice_client.hpp"
#include "d3d9_hook.hpp"
#include "dxgi_hook.hpp"
#include "dbglog.hpp"
#include "anti_tamper.hpp"
#include <Windows.h>
#include <cstdio>
#include <fstream>
#include <string>

// Overlay mode (read once at startup, before the D3D9 hook installs):
//   default            → in-process overlay (draws inside the game; captured by
//                        OBS Game Capture; safe for multi-boxing). In-game frame
//                        pacing is fine — the old "Pro Audio" thread stutter that
//                        once pushed people to the external window is fixed.
//   overlay_external: 1 → external Discord-style window (separate layered window;
//                        single-client only — a 2nd box gets killed by anti-cheat,
//                        and OBS Game Capture can't see it).
static bool read_overlay_external() {
    std::ifstream f("voice_client.conf");
    if (!f.is_open()) return false;            // default: in-process overlay
    std::string line;
    while (std::getline(f, line)) {
        auto cm = line.find("//");
        if (cm != std::string::npos) line = line.substr(0, cm);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key); trim(val);
        if (key == "overlay_external") return (val == "1" || val == "true");
    }
    return false;                              // key absent → default in-process
}

static bool read_overlay_enabled() {
    std::ifstream f("voice_client.conf");
    if (!f.is_open()) return true;             // default: overlay enabled
    std::string line;
    while (std::getline(f, line)) {
        auto cm = line.find("//");
        if (cm != std::string::npos) line = line.substr(0, cm);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key); trim(val);
        if (key == "overlay_enabled")
            return !(val == "0" || val == "false");
    }
    return true;
}

static std::string read_overlay_backend() {
    std::ifstream f("voice_client.conf");
    if (!f.is_open()) return "dx9";
    std::string line;
    while (std::getline(f, line)) {
        auto cm = line.find("//");
        if (cm != std::string::npos) line = line.substr(0, cm);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key); trim(val);
        if (key == "overlay_backend")
            return val;
    }
    return "dx9";
}

// Full-screen fill passes that used to steady RO's frame pacing. The real cause
// of the map-scroll stutter (audio threads on MMCSS "Pro Audio" preempting the
// game) is fixed now, so this workaround defaults to 0 (off). Left configurable
// via overlay_pacing_fill for any edge-case machine.
static int read_pacing_fill() {
    std::ifstream f("voice_client.conf");
    if (!f.is_open()) return 0;
    std::string line;
    while (std::getline(f, line)) {
        auto cm = line.find("//");
        if (cm != std::string::npos) line = line.substr(0, cm);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key); trim(val);
        if (key == "overlay_pacing_fill") {
            try { int n = std::stoi(val); return n < 0 ? 0 : n; } catch (...) { return 0; }
        }
    }
    return 0;
}

static int read_overlay_frame_interval() {
    std::ifstream f("voice_client.conf");
    if (!f.is_open()) return 1;
    std::string line;
    while (std::getline(f, line)) {
        auto cm = line.find("//");
        if (cm != std::string::npos) line = line.substr(0, cm);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key); trim(val);
        if (key == "overlay_frame_interval") {
            try {
                int n = std::stoi(val);
                if (n < 1) n = 1;
                if (n > 10) n = 10;
                return n;
            } catch (...) {
                return 1;
            }
        }
    }
    return 1;
}

static int read_dx9_present_fps_cap() {
    std::ifstream f("voice_client.conf");
    if (!f.is_open()) return 0;
    std::string line;
    while (std::getline(f, line)) {
        auto cm = line.find("//");
        if (cm != std::string::npos) line = line.substr(0, cm);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key); trim(val);
        if (key == "dx9_present_fps_cap") {
            try {
                int n = std::stoi(val);
                if (n < 0) n = 0;
                if (n > 240) n = 240;
                return n;
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

extern "C" __declspec(dllexport) void VoiceAttach() {}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x406D1388) return EXCEPTION_CONTINUE_SEARCH;

    void* addr = ep->ExceptionRecord->ExceptionAddress;

    HMODULE hMod = nullptr;
    char modname[MAX_PATH] = "<unknown>";
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr), &hMod))
        GetModuleFileNameA(hMod, modname, MAX_PATH);

    // Also log the exception info (access violation = read/write + bad address)
    char extra[64] = "";
    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2)
        sprintf_s(extra, " [%s addr=%p]",
            ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : "write",
            reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));

    char b[512];
    sprintf_s(b, "[CRASH] code=%08lX addr=%p tid=%lu mod=%s%s",
        code, addr, GetCurrentThreadId(), modname, extra);
    dbglog(b);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool g_initialized = false;

// ── Anti-tamper background thread ─────────────────────────────────────────
// Runs every 30 s; each tick does a full analysis-tool scan.
// We don't check more often — process enumeration is expensive and visible
// in profilers. 30 s is short enough to catch tools opened after injection.
static std::atomic<bool> g_at_running{ false };
static std::atomic<bool> g_shutdown_started{ false };
static HANDLE g_at_thread = nullptr;
static HANDLE g_main_thread = nullptr;

static DWORD WINAPI AntiTamperThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // Initial check right at startup (catches tools that were already open
    // before the DLL was injected).
    anti_tamper::periodic_check();

    while (g_at_running.load(std::memory_order_relaxed)) {
        // Interruptible ~30 s sleep: wake within ~100 ms once g_at_running goes
        // false so shutdown doesn't block waiting on a 30 s Sleep (which made the
        // game hang on close).
        for (int i = 0; i < 300 && g_at_running.load(std::memory_order_relaxed); ++i)
            Sleep(100);
        if (!g_at_running.load(std::memory_order_relaxed))
            break;
        anti_tamper::periodic_check();
    }
    return 0;
}

static DWORD WINAPI MainThread(LPVOID) {
    if (g_initialized) return 0; // ไม่ init ซ้ำ
    g_initialized = true;

    dbglog("MainThread started");
    Sleep(1000);
    if (g_shutdown_started.load())
        return 0;

    // Start anti-tamper monitor before any real work begins.
    g_at_running.store(true);
    g_at_thread = CreateThread(nullptr, 0, AntiTamperThread, nullptr, 0, nullptr);
    if (g_at_thread)
        SetThreadPriority(g_at_thread, THREAD_PRIORITY_BELOW_NORMAL);

    if (read_overlay_enabled()) {
        const std::string backend = read_overlay_backend();
        char backend_log[96];
        sprintf_s(backend_log, "overlay_backend=%s", backend.c_str());
        dbglog(backend_log);
        if (backend == "dx11") {
            dbglog("Calling DxgiHook::install (overlay_backend=dx11)");
            bool hook_ok = DxgiHook::install();
            dbglog(hook_ok ? "DxgiHook::install OK" : "DxgiHook::install FAILED");
        } else if (backend == "auto") {
            dbglog("Calling DxgiHook::install (overlay_backend=auto)");
            bool dxgi_ok = DxgiHook::install();
            dbglog(dxgi_ok ? "DxgiHook::install OK" : "DxgiHook::install FAILED");
            D3D9Hook::set_frame_interval(read_overlay_frame_interval());
            D3D9Hook::set_present_fps_cap(read_dx9_present_fps_cap());
            dbglog("Calling D3D9Hook::install (overlay_backend=auto fallback)");
            bool d3d9_ok = D3D9Hook::install();
            dbglog(d3d9_ok ? "D3D9Hook::install OK" : "D3D9Hook::install FAILED");
        } else {
        // Overlay mode: default is the in-process overlay (visible to streamers).
        // Only start the external Discord-style overlay if explicitly requested in
        // voice_client.conf (overlay_external: 1). Must start before the D3D9 hook so
        // it can claim UI ownership before the first rendered frame.
        if (read_overlay_external()) {
            ExternalOverlay::start();
            dbglog("ExternalOverlay::start called (overlay_external=1)");
        } else {
            dbglog("Using in-process overlay (default)");
        }

        // Frame-pacing stabilizer for the in-process overlay (fixes RO map-scroll
        // stutter). Harmless for the external overlay (it gates on the in-process
        // device anyway). Configurable via overlay_pacing_fill (default 1).
        Overlay::set_pacing_fill(read_pacing_fill());
        D3D9Hook::set_frame_interval(read_overlay_frame_interval());
        D3D9Hook::set_present_fps_cap(read_dx9_present_fps_cap());

        dbglog("Calling D3D9Hook::install");
        bool hook_ok = D3D9Hook::install();
        dbglog(hook_ok ? "D3D9Hook::install OK" : "D3D9Hook::install FAILED");
        }
    } else {
        dbglog("Overlay disabled by config (overlay_enabled=0); skipping D3D9 hook");
    }
    if (g_shutdown_started.load())
        return 0;

    dbglog("Calling VoiceClient::init");
    VoiceClient::get().init();
    dbglog("VoiceClient::init done");

    return 0;
}

// Stop every background thread we spawned (anti-tamper, overlay, voice/audio/
// network) so the OS doesn't force-kill them mid heap/COM call on process exit —
// which deadlocks the loader/heap lock during teardown and HANGS the process.
// Safe to call multiple times; does NOT touch the d3d9/wndproc hooks (left for
// the natural process teardown, which is harmless once the threads are gone).
// MUST NOT be called from one of these threads themselves (it joins them).
static std::atomic<bool> g_threads_stopped{ false };

extern "C" __declspec(dllexport) void VoiceStopThreads() {
    if (g_threads_stopped.exchange(true))
        return;

    g_at_running.store(false);
    if (g_at_thread) {
        WaitForSingleObject(g_at_thread, 2000);  // wakes within ~100ms now
        CloseHandle(g_at_thread);
        g_at_thread = nullptr;
    }

    ExternalOverlay::stop();
    VoiceClient::get().shutdown();
}

static void ShutdownVoiceClient() {
    if (g_shutdown_started.exchange(true))
        return;

    if (g_main_thread && GetThreadId(g_main_thread) != GetCurrentThreadId()) {
        WaitForSingleObject(g_main_thread, 5000);
        CloseHandle(g_main_thread);
        g_main_thread = nullptr;
    }

    VoiceStopThreads();
    D3D9Hook::uninstall();
}

extern "C" __declspec(dllexport) void VoiceDetach() {
    ShutdownVoiceClient();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpvReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        AddVectoredExceptionHandler(1, CrashHandler);
        g_main_thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        dbglog("DLL_PROCESS_DETACH");
        // DllMain runs under the loader lock; keep detach non-blocking.
        // Explicit unloaders should call VoiceDetach() before FreeLibrary.
        g_at_running.store(false);
    }
    return TRUE;
}
