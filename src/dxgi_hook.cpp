#include "dxgi_hook.hpp"
#include "voice_client.hpp"
#include "dbglog.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static std::atomic<bool> g_installed{ false };
static Present_t g_present = nullptr;
static Present1_t g_present1 = nullptr;
static ResizeBuffers_t g_resize = nullptr;
static unsigned char g_present_original[5]{};
static unsigned char g_present1_original[5]{};
static unsigned char g_resize_original[5]{};
static void* g_present_trampoline = nullptr;
static void* g_present1_trampoline = nullptr;
static void* g_resize_trampoline = nullptr;

static HWND g_hwnd = nullptr;
static WNDPROC g_old_wndproc = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static bool g_imgui = false;

static bool write_jmp(void* target, void* hook, unsigned char saved[5], void** trampoline) {
    if (!target || !hook || !saved || !trampoline) return false;
    memcpy(saved, target, 5);

    unsigned char* gate = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!gate) return false;
    memcpy(gate, saved, 5);
    gate[5] = 0xE9;
    int32_t back = static_cast<int32_t>(
        (reinterpret_cast<uintptr_t>(target) + 5) - (reinterpret_cast<uintptr_t>(gate) + 10));
    memcpy(gate + 6, &back, 4);
    *trampoline = gate;

    DWORD old = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    unsigned char patch[5]{ 0xE9, 0, 0, 0, 0 };
    int32_t rel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(hook) - (reinterpret_cast<uintptr_t>(target) + 5));
    memcpy(patch + 1, &rel, 4);
    memcpy(target, patch, 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    return true;
}

static void restore_jmp(void* target, unsigned char saved[5], void* trampoline) {
    if (!target) return;
    DWORD old = 0;
    if (VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(target, saved, 5);
        VirtualProtect(target, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), target, 5);
    }
    if (trampoline) VirtualFree(trampoline, 0, MEM_RELEASE);
}

static HWND find_main_window() {
    struct FindData { DWORD pid; HWND hwnd; } data{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* d = reinterpret_cast<FindData*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == d->pid && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) {
            d->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    return data.hwnd;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imgui && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return TRUE;
    return CallWindowProcW(g_old_wndproc, hwnd, msg, wp, lp);
}

static void release_rtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

static bool create_rtv(IDXGISwapChain* sc) {
    release_rtv();
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) || !backbuffer)
        return false;
    HRESULT hr = g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
    backbuffer->Release();
    return SUCCEEDED(hr);
}

static bool init_imgui(IDXGISwapChain* sc) {
    if (g_imgui) return true;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) {
        dbglog("[dxgi] GetDevice(ID3D11Device) failed");
        return false;
    }
    g_device->GetImmediateContext(&g_ctx);
    if (!g_ctx) {
        dbglog("[dxgi] GetImmediateContext failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_hwnd = desc.OutputWindow ? desc.OutputWindow : find_main_window();
    if (!g_hwnd) {
        dbglog("[dxgi] hwnd not found");
        return false;
    }
    if (!create_rtv(sc)) {
        dbglog("[dxgi] create RTV failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0E00, 0x0E7F, 0 };
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 14.0f, nullptr, ranges))
        io.Fonts->AddFontDefault();

    if (!ImGui_ImplWin32_Init(g_hwnd)) {
        dbglog("[dxgi] ImGui_ImplWin32_Init failed");
        return false;
    }
    if (!ImGui_ImplDX11_Init(g_device, g_ctx)) {
        dbglog("[dxgi] ImGui_ImplDX11_Init failed");
        ImGui_ImplWin32_Shutdown();
        return false;
    }

    g_old_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc)));
    g_imgui = true;
    dbglog("[dxgi] ImGui DX11 initialized");
    return true;
}

static void draw_minimal_voice_ui() {
    VoiceClient& vc = VoiceClient::get();

    const bool on_map = vc.is_on_map();
    const bool ready = vc.is_auth_confirmed();
    const bool muted = vc.is_muted();
    const bool talking = vc.is_ptt_active() && vc.is_locally_talking();

    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("VoiceDX11", nullptr, flags)) {
        ImVec4 col = !on_map ? ImVec4(0.75f, 0.75f, 0.75f, 1.0f)
                    : !ready ? ImVec4(0.35f, 0.55f, 1.0f, 1.0f)
                    : muted ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                    : talking ? ImVec4(0.35f, 1.0f, 0.55f, 1.0f)
                    : ImVec4(0.75f, 0.85f, 0.95f, 1.0f);
        ImGui::TextColored(col, "%s", !on_map ? "Voice DX11: hook active" : !ready ? "Voice: connecting" : muted ? "Voice: muted" : talking ? "Voice: talking" : "Voice: ready");
    }
    ImGui::End();
}

static void render_dx11_overlay(IDXGISwapChain* sc) {
    if (init_imgui(sc) && g_rtv) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_minimal_voice_ui();
        ImGui::Render();
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    static bool logged = false;
    if (!logged) { dbglog("[dxgi] Present hit"); logged = true; }
    render_dx11_overlay(sc);
    return reinterpret_cast<Present_t>(g_present_trampoline)(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    static bool logged = false;
    if (!logged) { dbglog("[dxgi] Present1 hit"); logged = true; }
    render_dx11_overlay(sc);
    return reinterpret_cast<Present1_t>(g_present1_trampoline)(sc, sync, flags, params);
}

static HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    release_rtv();
    if (g_imgui) ImGui_ImplDX11_InvalidateDeviceObjects();
    HRESULT hr = reinterpret_cast<ResizeBuffers_t>(g_resize_trampoline)(sc, count, w, h, fmt, flags);
    if (SUCCEEDED(hr) && g_device) {
        create_rtv(sc);
        if (g_imgui) ImGui_ImplDX11_CreateDeviceObjects();
    }
    return hr;
}

static bool get_dxgi_methods(void** present, void** present1, void** resize) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VoiceDxgiDummyWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    }
    if (FAILED(hr) || !sc) {
        DestroyWindow(hwnd);
        return false;
    }
    void** vt = *reinterpret_cast<void***>(sc);
    *present = vt[8];
    *resize = vt[13];
    *present1 = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
        void** vt1 = *reinterpret_cast<void***>(sc1);
        *present1 = vt1[22];
        sc1->Release();
    }
    sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    return true;
}

} // namespace

bool DxgiHook::install() {
    if (g_installed.load()) return true;
    void* present = nullptr;
    void* present1 = nullptr;
    void* resize = nullptr;
    if (!get_dxgi_methods(&present, &present1, &resize)) {
        dbglog("[dxgi] failed to get DXGI methods");
        return false;
    }
    g_present = reinterpret_cast<Present_t>(present);
    g_present1 = reinterpret_cast<Present1_t>(present1);
    g_resize = reinterpret_cast<ResizeBuffers_t>(resize);
    char b[160];
    sprintf_s(b, "[dxgi] methods present=%p present1=%p resize=%p", present, present1, resize);
    dbglog(b);
    if (!write_jmp(present, reinterpret_cast<void*>(&hk_present), g_present_original, &g_present_trampoline))
        return false;
    if (present1 && present1 != present) {
        if (!write_jmp(present1, reinterpret_cast<void*>(&hk_present1), g_present1_original, &g_present1_trampoline)) {
            restore_jmp(present, g_present_original, g_present_trampoline);
            return false;
        }
    }
    if (!write_jmp(resize, reinterpret_cast<void*>(&hk_resize), g_resize_original, &g_resize_trampoline)) {
        restore_jmp(present, g_present_original, g_present_trampoline);
        if (present1 && present1 != present)
            restore_jmp(present1, g_present1_original, g_present1_trampoline);
        return false;
    }
    g_installed.store(true);
    dbglog("[dxgi] hook installed");
    return true;
}

void DxgiHook::uninstall() {
    if (!g_installed.exchange(false)) return;
    restore_jmp(reinterpret_cast<void*>(g_present), g_present_original, g_present_trampoline);
    if (g_present1 && reinterpret_cast<void*>(g_present1) != reinterpret_cast<void*>(g_present))
        restore_jmp(reinterpret_cast<void*>(g_present1), g_present1_original, g_present1_trampoline);
    restore_jmp(reinterpret_cast<void*>(g_resize), g_resize_original, g_resize_trampoline);
    if (g_imgui) {
        if (g_old_wndproc && g_hwnd)
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_old_wndproc));
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    release_rtv();
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_imgui = false;
}

bool DxgiHook::is_installed() {
    return g_installed.load();
}
