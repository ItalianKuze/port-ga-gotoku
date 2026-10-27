// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>
#include "bbport_game.h"
#include "bbport_perf.h"
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "pgg_env.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license), and Figtree.
#ifdef _WIN32
// COFF names the read-only section .rdata and has no .hidden. The explicit .section .text at the
// end is the counterpart of ELF's .previous: without it the assembler still considers itself
// inside .rdata and rejects the .seh_endproc GCC emits next.
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".globl bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".globl bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".balign 16\n"
    ".globl bb_ui_font_ttf\n"
    "bb_ui_font_ttf:\n"
    ".incbin \"" BB_UI_FONT_PATH "\"\n"
    ".globl bb_ui_font_ttf_end\n"
    "bb_ui_font_ttf_end:\n"
    ".section .text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".balign 16\n"
    ".hidden bb_ui_font_ttf\n"
    ".global bb_ui_font_ttf\n"
    "bb_ui_font_ttf:\n"
    ".incbin \"" BB_UI_FONT_PATH "\"\n"
    ".hidden bb_ui_font_ttf_end\n"
    ".global bb_ui_font_ttf_end\n"
    "bb_ui_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
// PortGaGotoku: Figtree (third_party/fonts, SIL OFL 1.1), the launcher's font, for Hokuto's menu.
extern "C" const unsigned char bb_ui_font_ttf[];
extern "C" const unsigned char bb_ui_font_ttf_end[];


namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;

// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (value) {
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// PortGaGotoku: the places of the next phases on Hokuto: the statistics HUD (frame times, CPU
// translation against GPU time) and Hokuto's own settings.
void HokutoSections(BbSettings::Values& s) {
    ImGui::SeparatorText(BbSettings::MenuText("Statistics (HUD)"));
    Checkbox(BbSettings::MenuText("Show statistics (F11)"), s.show_hud);
    if (const auto [done, length] = BbPerf::MeasurementProgress(); done < 0.0f) {
        if (ImGui::Button(BbSettings::MenuText("Start measurement (90 s, F9)"))) {
            BbPerf::StartMeasurement(90);
        }
        Hint(BbSettings::MenuText(
            "Records 90 s of frame times, CPU and GPU time and thread load into the logs folder (perf-*.csv)."));
    } else {
        ImGui::TextColored(ImVec4(0.89f, 0.70f, 0.35f, 1.0f),
                           BbSettings::MenuText("Measuring: %.0f / %d s"), done, length);
    }
    ImGui::SeparatorText(BbSettings::MenuText("Game settings"));
    ImGui::TextDisabled("%s", BbSettings::MenuText("Hokuto's own settings will be here"));
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Where it was moved last (pgg.ini menu_pos, a fraction of the screen), kept on screen.
    ImVec2 pos(viewport->WorkPos.x + 40.0f * base_scale, viewport->WorkPos.y + 40.0f * base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * base_scale;
        pos.x = viewport->WorkPos.x +
                std::clamp(s.menu_x * viewport->WorkSize.x, 0.0f, std::max(viewport->WorkSize.x - margin, 0.0f));
        pos.y = viewport->WorkPos.y +
                std::clamp(s.menu_y * viewport->WorkSize.y, 0.0f, std::max(viewport->WorkSize.y - margin, 0.0f));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin("PortGaGotoku  (Insert / L3+R3)###bbport_settings", &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // Moved: remembered (saved with the settings when the menu closes).
    if (!ImGui::IsWindowAppearing() && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - viewport->WorkPos.x) / viewport->WorkSize.x;
        const float fy = (at.y - viewport->WorkPos.y) / viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            dirty = true;
        }
    }
    // Always readable, whatever the menu's language.
    static const char* languages[] = {"English", "Italiano"};
    int language = s.menu_language == BbSettings::MenuLanguage::Italian ? 1 : 0;
    if (ImGui::Combo("Language / Lingua", &language, languages, 2)) {
        s.menu_language = language == 1 ? BbSettings::MenuLanguage::Italian : BbSettings::MenuLanguage::English;
        BbSettings::Save();
    }
    ImGui::Text(BbSettings::MenuText("%.0f FPS  (%.1f ms)"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);


    HokutoSections(s);

    ImGui::SeparatorText(BbSettings::MenuText("Other"));
    Checkbox(BbSettings::MenuText("FPS counter in corner"), s.show_fps);

    ImGui::Spacing();
    if (ImGui::Button(BbSettings::MenuText("Close"))) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", BbSettings::MenuText("Settings are saved to pgg.ini"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%.0f FPS  %.1f ms", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);
    ImGui::End();
}

// PortGaGotoku: the statistics HUD (F11 or the menu), top left. bbport_perf gathers the values.
void HudOverlay() {
    const BbPerf::Snapshot p = BbPerf::GetSnapshot();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::Begin("##bb_hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    const ImVec4 strong(0.965f, 0.937f, 0.894f, 1.0f), muted(0.66f, 0.62f, 0.55f, 1.0f);
    const ImVec4 warn(0.89f, 0.70f, 0.35f, 1.0f), bad(0.95f, 0.65f, 0.61f, 1.0f);
    ImGui::TextColored(strong, "%.0f FPS", p.fps);
    ImGui::SameLine();
    ImGui::TextColored(muted, "%.1f ms   1%% low %.0f", p.frame_ms, p.low1_fps);
    if (!p.frame_history.empty()) {
        float worst = 0.0f;
        for (float v : p.frame_history) {
            worst = std::max(worst, v);
        }
        char label[32];
        std::snprintf(label, sizeof(label), "max %.1f ms", worst);
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.70f, 0.13f, 0.16f, 1.0f));
        ImGui::PlotLines("##frametimes", p.frame_history.data(), int(p.frame_history.size()), 0, label, 0.0f,
                         std::max(40.0f, worst * 1.1f), ImVec2(260.0f * base_scale, 46.0f * base_scale));
        ImGui::PopStyleColor();
    }
    char cpu[24] = "n/a", gpu[24] = "n/a";
    if (p.cpu_translate_ms >= 0.0f) std::snprintf(cpu, sizeof(cpu), "%.2f ms", p.cpu_translate_ms);
    if (p.gpu_ms >= 0.0f) std::snprintf(gpu, sizeof(gpu), "%.2f ms", p.gpu_ms);
    ImGui::Text(BbSettings::MenuText("CPU translation %s  GPU %s"), cpu, gpu);
    if (p.gpu_util >= 0.0f) {
        ImGui::Text("GPU %.0f%%  VRAM %.0f / %.0f MB", p.gpu_util, p.vram_used_mb, p.vram_budget_mb);
    } else {
        ImGui::Text("VRAM %.0f / %.0f MB", p.vram_used_mb, p.vram_budget_mb);
    }
    ImGui::Text("CPU %.0f%%  RAM %.1f / %.1f GB", p.cpu_util, p.ram_used_gb, p.ram_total_gb);
    ImGui::TextColored(p.compiles_last_5s ? warn : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                       BbSettings::MenuText("Pipeline compiles: %u in 5 s, %llu total"),
                       p.compiles_last_5s, static_cast<unsigned long long>(p.compiles_total));
    if (!p.top_threads.empty()) {
        ImGui::TextColored(muted, "%s", BbSettings::MenuText("Busiest threads:"));
        for (const auto& t : p.top_threads) {
            ImGui::TextColored(t.percent >= 90.0f ? bad : muted, "  %3.0f%%  %s", t.percent, t.name.c_str());
        }
    }
    ImGui::TextColored(muted, "%s", p.gpu_name.c_str());
    if (const auto [done, length] = BbPerf::MeasurementProgress(); done >= 0.0f) {
        ImGui::TextColored(warn, BbSettings::MenuText("REC %.0f / %d s"), done, length);
    }
    ImGui::End();
}

// While a measurement records with the HUD hidden: a small REC label in the corner.
void RecordingLabel() {
    const auto [done, length] = BbPerf::MeasurementProgress();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##bb_rec", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.89f, 0.70f, 0.35f, 1.0f), BbSettings::MenuText("REC %.0f / %d s"),
                       done, length);
    ImGui::End();
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel");
    ImGui::End();
}

} // namespace

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

// PortGaGotoku: the launcher's look (launcher/ui/app.css): dark warm ground, bone text, crimson for
// what is on or chosen.
static void HokutoStyle(ImGuiStyle& style) {
    const auto rgb = [](unsigned hex, float alpha = 1.0f) {
        return ImVec4(float((hex >> 16) & 0xff) / 255.0f, float((hex >> 8) & 0xff) / 255.0f,
                      float(hex & 0xff) / 255.0f, alpha);
    };
    style.WindowRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.WindowPadding = ImVec2(18.0f, 16.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 9.0f);
    style.WindowBorderSize = 1.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = rgb(0xece3d3);
    c[ImGuiCol_TextDisabled] = rgb(0x93877a);
    c[ImGuiCol_WindowBg] = rgb(0x161311, 0.95f);
    c[ImGuiCol_PopupBg] = rgb(0x161311, 0.98f);
    c[ImGuiCol_Border] = rgb(0x2b2521);
    c[ImGuiCol_FrameBg] = rgb(0x0f0d0c);
    c[ImGuiCol_FrameBgHovered] = rgb(0x2c2622);
    c[ImGuiCol_FrameBgActive] = rgb(0x3a332e);
    c[ImGuiCol_TitleBg] = rgb(0x100e0d);
    c[ImGuiCol_TitleBgActive] = rgb(0x1f1a17);
    c[ImGuiCol_TitleBgCollapsed] = rgb(0x100e0d);
    c[ImGuiCol_CheckMark] = rgb(0xffffff);
    c[ImGuiCol_SliderGrab] = rgb(0xb3222a);
    c[ImGuiCol_SliderGrabActive] = rgb(0xc4313a);
    c[ImGuiCol_Button] = rgb(0x2c2622);
    c[ImGuiCol_ButtonHovered] = rgb(0x3a332e);
    c[ImGuiCol_ButtonActive] = rgb(0xb3222a);
    c[ImGuiCol_Header] = rgb(0x2c2622);
    c[ImGuiCol_HeaderHovered] = rgb(0x3a332e);
    c[ImGuiCol_HeaderActive] = rgb(0xb3222a);
    c[ImGuiCol_Separator] = rgb(0x2b2521);
    c[ImGuiCol_SeparatorHovered] = rgb(0x3a332e);
    c[ImGuiCol_SeparatorActive] = rgb(0xb3222a);
    c[ImGuiCol_ResizeGrip] = rgb(0x2c2622);
    c[ImGuiCol_ResizeGripHovered] = rgb(0x3a332e);
    c[ImGuiCol_ResizeGripActive] = rgb(0xb3222a);
    c[ImGuiCol_NavCursor] = rgb(0xcfc4b2);
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "PortGaGotoku";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    if (BbGame::IsHokuto()) {
        HokutoStyle(style);
    }

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    if (BbGame::IsHokuto()) {
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_ui_font_ttf),
                                       int(bb_ui_font_ttf_end - bb_ui_font_ttf), 19.0f, &font_config);
    } else {
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
    }

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    BbPerf::Init(instance);
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        // PortGaGotoku: F11 shows or hides the statistics HUD, F9 starts a 90 s measurement.
        if (down && !event.key.repeat && event.key.key == SDLK_F11) {
            auto& settings = BbSettings::Get();
            settings.show_hud = !settings.show_hud.load();
            BbSettings::Save();
            return true;
        }
        // While the pad is recorded (PGG_PAD_RECORD), F9 is the recording's key.
        static const bool pad_recording = pgg_getenv("PAD_RECORD") && *pgg_getenv("PAD_RECORD");
        if (down && !event.key.repeat && event.key.key == SDLK_F9 && !pad_recording) {
            BbPerf::StartMeasurement(90);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || prompt_active || BbSettings::Get().show_fps || BbSettings::Get().show_hud ||
                           BbPerf::MeasurementProgress().first >= 0.0f);
}

bool MenuOpen() {
    return menu_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || prompt_active;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    BbPerf::SetHudVisible(BbSettings::Get().show_hud);
    BbPerf::OnPresent(ms);
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_hud && !menu_open) {
        HudOverlay();
    } else if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (!BbSettings::Get().show_hud && !menu_open && BbPerf::MeasurementProgress().first >= 0.0f) {
        RecordingLabel();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
