// overlay.cpp - the F1 options overlay: Dear ImGui (third_party/imgui, tools/get_imgui.ps1) over the paused game,
// on the SDL_Renderer the game presents with. See overlay.h. Edits settings.h's options; the host applies them.
#include <time.h>
extern "C" {
#include "savestate.h"
}
#include "overlay.h"
#include "settings.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#ifndef CT_NO_GL                                           // (the Switch build: SDL_Renderer only, no ImGui GL loader)
#include "backends/imgui_impl_opengl3.h"
#endif
#ifdef CT_HAVE_VULKAN
#include "backends/imgui_impl_vulkan.h"
#include "present_vk.h"
#endif
#include <stdio.h>
#include <string.h>

static SDL_Renderer* g_ren;
static int g_ok;
static int g_gl;                                           // the OpenGL backend (else SDL_Renderer)
static CtVk* g_vk;                                         // the Vulkan backend
#ifdef CT_HAVE_VULKAN
static PFN_vkVoidFunction vkLoader(const char* name, void* user) { return (PFN_vkVoidFunction)CtVkProc((CtVk*)user, name); }
#endif
static char g_backendName[160];
static char g_padName[128];
// a binding being captured: the button, and which slot (0 / 1 = keyboard keys, 2 = the controller)
static int g_capBtn = -1, g_capSlot = 0;
static int g_selTab = -1;                                  // CtOverlaySelectTab: the tab to open on
extern "C" void CtOverlaySelectTab(int tab) { g_selTab = tab; }

extern "C" int CtOverlayAvailable(void) { return g_ok; }

extern "C" int CtOverlayInit(CtPresenter* pr) {
  if (g_ok) return 1;
  SDL_Window* win = CtPresentWindow(pr);
  SDL_Renderer* ren = CtPresentSdlRenderer(pr);
  void* glc = CtPresentGlContext(pr);
  CtVk* vk = CtPresentVk(pr);
  if (!win || (!ren && !glc && !vk)) return 0;
  snprintf(g_backendName, sizeof g_backendName, "%s", CtPresentName(pr));
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = NULL;                                   // the options live in ct_settings.ini, not imgui.ini
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  // The built-in bitmap font at a whole multiple of its 13 px, from the window's height: sharp at 2x on a 1080p
  // fullscreen, 1x in the default 512x448 window.
  { int ow = 0, oh = 0; CtPresentOutputSize(pr, &ow, &oh);
    int k = (oh + 270) / 540; if (k < 1) k = 1; if (k > 4) k = 4;
#ifdef __SWITCH__
    if (k < 2) k = 2;                                      // a 720p handheld screen read from arm's length: 13 px is too small
#endif
    ImFontConfig fc; fc.SizePixels = 13.0f * (float)k; io.Fonts->AddFontDefault(&fc);
    ImGui::GetStyle().ScaleAllSizes((float)k); }
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 6.0f; st.FrameRounding = 4.0f; st.GrabRounding = 4.0f;
  bool ok;
#ifdef CT_HAVE_VULKAN
  if (vk) {
    CtVkInfo vi; CtVkInfoGet(vk, &vi);
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.Instance = (VkInstance)vi.instance; ii.PhysicalDevice = (VkPhysicalDevice)vi.physicalDevice;
    ii.Device = (VkDevice)vi.device; ii.QueueFamily = vi.queueFamily; ii.Queue = (VkQueue)vi.queue;
    ii.DescriptorPool = (VkDescriptorPool)vi.descriptorPool; ii.RenderPass = (VkRenderPass)vi.renderPass;
    ii.MinImageCount = vi.minImageCount; ii.ImageCount = vi.imageCount; ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ok = ImGui_ImplSDL2_InitForVulkan(win) && ImGui_ImplVulkan_LoadFunctions(vkLoader, vk) && ImGui_ImplVulkan_Init(&ii);
  } else
#endif
#ifndef CT_NO_GL
  ok = glc ? (ImGui_ImplSDL2_InitForOpenGL(win, glc) && ImGui_ImplOpenGL3_Init("#version 330 core"))
           : (ImGui_ImplSDL2_InitForSDLRenderer(win, ren) && ImGui_ImplSDLRenderer2_Init(ren));
#else
  glc = NULL;
  ok = ImGui_ImplSDL2_InitForSDLRenderer(win, ren) && ImGui_ImplSDLRenderer2_Init(ren);
#endif
  if (!ok) { ImGui::DestroyContext(); return 0; }
  g_ren = ren; g_gl = glc != NULL; g_vk = vk; g_ok = 1;
  return 1;
}

extern "C" void CtOverlayShutdown(void) {
  if (!g_ok) return;
#ifdef CT_HAVE_VULKAN
  if (g_vk) ImGui_ImplVulkan_Shutdown(); else
#endif
#ifndef CT_NO_GL
  if (g_gl) ImGui_ImplOpenGL3_Shutdown(); else
#endif
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
  g_ok = 0;
}

extern "C" void CtOverlaySetPadName(const char* name) {
  snprintf(g_padName, sizeof g_padName, "%s", name ? name : "");
}

extern "C" int CtOverlayProcessEvent(const SDL_Event* e) {
  if (!g_ok) return 0;
  if (g_capBtn >= 0) {                                     // binding: the next key / controller button is the answer
    CtSettings* s = CtSettingsGet();
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
      if (e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) { g_capBtn = -1; return 1; }   // cancel
      if (g_capSlot < 2) s->keys[g_capBtn][g_capSlot] = (int)e->key.keysym.scancode;
      g_capBtn = -1;
      return 1;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN && g_capSlot == 2) {
      s->pad[g_capBtn] = e->cbutton.button;
      g_capBtn = -1;
      return 1;
    }
    if (e->type == SDL_KEYUP || e->type == SDL_CONTROLLERBUTTONUP) return 1;
  }
  // TOUCH. The console's SDL reports a touch as finger events and synthesises mouse events from them, but those carry no window (the console has no mouse focus), which
  // the ImGui backend drops. So a finger drives the pointer here, from its normalised position, and the synthesised mouse events are ignored (no double input).
  if (e->type == SDL_FINGERDOWN || e->type == SDL_FINGERMOTION || e->type == SDL_FINGERUP) {
    ImGuiIO& tio = ImGui::GetIO();
    static SDL_FingerID active = 0; static int have = 0;
    if (e->type == SDL_FINGERDOWN && !have) { active = e->tfinger.fingerId; have = 1; }
    if (have && e->tfinger.fingerId == active) {
      tio.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
      tio.AddMousePosEvent(e->tfinger.x * tio.DisplaySize.x, e->tfinger.y * tio.DisplaySize.y);
      if (e->type == SDL_FINGERDOWN) tio.AddMouseButtonEvent(0, true);
      if (e->type == SDL_FINGERUP) { tio.AddMouseButtonEvent(0, false); have = 0; }
    }
    return 1;
  }
  if ((e->type == SDL_MOUSEMOTION && e->motion.which == SDL_TOUCH_MOUSEID) ||
      ((e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP) && e->button.which == SDL_TOUCH_MOUSEID)) return 1;
  ImGui_ImplSDL2_ProcessEvent(e);
  const ImGuiIO& io = ImGui::GetIO();
  if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) && io.WantTextInput) return 1;
  return 0;
}

// The controller button as the player reads it: upper case, the d-pad and shoulders spelt as on the console.
static const char* padName(int b) {
  static char buf[8][40]; static int rot;
  if (b < 0) return "-";
  const char* n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
  if (!n) return "?";
  if (!strcmp(n, "dpup")) return "DPAD-UP";
  if (!strcmp(n, "dpdown")) return "DPAD-DOWN";
  if (!strcmp(n, "dpleft")) return "DPAD-LEFT";
  if (!strcmp(n, "dpright")) return "DPAD-RIGHT";
  if (!strcmp(n, "leftshoulder")) return "L";
  if (!strcmp(n, "rightshoulder")) return "R";
  char* o = buf[rot++ & 7]; size_t i = 0;
  for (; n[i] && i < sizeof buf[0] - 1; i++) o[i] = (char)((n[i] >= 'a' && n[i] <= 'z') ? n[i] - 32 : n[i]);
  o[i] = 0; return o;
}
// One binding cell: shows the controller button; a click captures a new one, a right click clears it.
static void bindCell(int btn, const char* label) {
  CtSettings* s = CtSettingsGet();
  char id[64];
  int capturing = g_capBtn == btn;
  snprintf(id, sizeof id, "%s##b%d", capturing ? "...PRESS A BUTTON..." : label, btn);
  if (ImGui::Button(id, ImVec2(-1, 0))) { g_capBtn = btn; g_capSlot = 2; }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    s->pad[btn] = -1;
    if (capturing) g_capBtn = -1;
  }
}

extern "C" int CtOverlayFrame(void) {
  if (!g_ok) return CT_OVL_CLOSE;
  int ch = 0;
  CtSettings* s = CtSettingsGet();
#ifdef CT_HAVE_VULKAN
  if (g_vk) ImGui_ImplVulkan_NewFrame(); else
#endif
#ifndef CT_NO_GL
  if (g_gl) ImGui_ImplOpenGL3_NewFrame(); else
#endif
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 size(vp->WorkSize.x * 0.8f, vp->WorkSize.y * 0.85f);
  if (size.x < 420) size.x = vp->WorkSize.x; if (size.y < 320) size.y = vp->WorkSize.y;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  bool open = true;
  ImGui::Begin("Options", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
               ImGuiWindowFlags_NoCollapse);
  if (!open) ch |= CT_OVL_CLOSE;
  if (ImGui::BeginTabBar("tabs")) {
    if (ImGui::BeginTabItem("Video", NULL, g_selTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
      const char* scales[] = { "Fit to window", "Integer scale (sharp pixels)", "Stretch" };
      if (ImGui::Combo("Scale", &s->scaleMode, scales, 3)) ch |= CT_OVL_VIDEO;
      const char* aspects[] = { "Square pixels (8:7)", "TV 4:3 (as on the console)" };
      if (ImGui::Combo("Aspect ratio", &s->aspect, aspects, 2)) ch |= CT_OVL_VIDEO;
      const char* filters[] = { "Sharp (nearest)", "Smooth (linear)" };
      if (ImGui::Combo("Filter", &s->linearFilter, filters, 2)) ch |= CT_OVL_VIDEO;
      const char* wides[] = { "Off (256 columns, as on the SNES)", "16:9", "16:10", "21:9" };
      if (ImGui::Combo("Widescreen", &s->widescreen, wides, 4)) ch |= CT_OVL_VIDEO;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The field, the world map, battles, the title and the special sequences (the race, the gate) show\n"
                          "more picture at the sides; menus and boxes stay in the middle. Outside the original camera's\n"
                          "area the game runs nothing extra: an object seen only in the extra strip is animated in the\n"
                          "picture only. In a room wider than the screen the picture stops at the room's edges (no black\n"
                          "strip) and the text box and the HUD stay in the middle.");
      bool rw = s->wideRoomWrap != 0;
      if (ImGui::Checkbox("Narrow rooms: continue the map at the sides", &rw)) s->wideRoomWrap = rw;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("In a narrow room as wide as its own map (the sky of the fight with Flea, GATO's room, the Telepod)\n"
                          "the sides show the map wrapping round, as the camera would see it -- sometimes repeating what\n"
                          "is in the middle. Off: black past the room's edges. A room inside a larger map (Crono's\n"
                          "house) is always black past its edge: other rooms are there.");
      bool ip = s->interpolate != 0;
      if (ImGui::Checkbox("Frames between (60+ fps)", &ip)) s->interpolate = ip;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game still runs at 60 frames per second; the screen is drawn at the monitor's rate, with the\n"
                          "camera and the field's objects between two frames (one frame of delay, ~16 ms). It only has an\n"
                          "effect on a monitor above 60 Hz (or without VSync).");
      ImGui::SliderInt("Internal resolution", &s->renderScale, 1, 4, "%dx");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The field's picture drawn at %dx%d: objects and the camera get positions between the SNES's\n"
                          "pixels. With frames between, 1x uses 4x.", 256 * s->renderScale, 224 * s->renderScale);
      bool vs = s->vsync != 0;
      if (ImGui::Checkbox("VSync", &vs)) { s->vsync = vs; ch |= CT_OVL_VIDEO; }
      if (!s->fullscreen && ImGui::SliderInt("Window size", &s->windowScale, 1, 8, "%dx")) ch |= CT_OVL_VIDEO;
#if defined(__SWITCH__) || defined(CT_CLK_PREVIEW)      // (CT_CLK_PREVIEW: draw it on a PC build, to look at the layout)
      { // THE SWITCH'S CLOCKS: CPU / GPU / memory, applied live by the host (switch_platform.c) and re-asserted while the game runs.
        const char* clk[] = { "Base (safe, stock)", "Erista boost", "Mariko boost", "Max performance" };
        int cs = s->clocks < 0 ? CT_CLK_ERISTA : s->clocks > 3 ? 3 : s->clocks;
        if (ImGui::Combo("Clocks", &cs, clk, 4)) s->clocks = cs;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Base (safe, stock): CPU 1020 MHz, GPU 460 MHz, memory 1331 MHz. 4x resolution close to 60fps.\n"
                            "Erista boost: GPU 618 MHz, memory 1600 MHz (CPU 1785). 4x resolution, ~60fps locked; generally regarded as safe levels.\n"
                            "Mariko boost: GPU 768 MHz, memory 1600 MHz (CPU 1785). 4x resolution, ~60fps locked; generally regarded as safe levels.\n"
                            "Max performance: CPU 1963 MHz, GPU 921 MHz, memory 1600 MHz. May degrade battery life; likely\n"
                            "unnecessary as boost clocks reach 60fps.\n"
                            "Takes effect at once. The Erista / Mariko choice follows the console's hardware by default.");
        ImGui::TextDisabled("%s", cs == CT_CLK_BASE ? "Safe, stock clocks: 4x resolution close to 60fps." :
                                  cs == CT_CLK_MAX ? "May degrade battery life; likely unnecessary as boost clocks reach 60fps." : "Generally regarded as safe: 4x resolution, ~60fps locked.");
      }
#endif
      const char* huds[] = { "Full HUD", "FPS only", "OFF" };          // (listed top to bottom; the setting counts the other way)
      int hudSel = 2 - (s->statsHud < 0 ? 0 : s->statsHud > 2 ? 2 : s->statsHud);
      if (ImGui::Combo("Performance HUD", &hudSel, huds, 3)) s->statsHud = 2 - hudSel;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Full HUD: the host's counters over the picture -- engine and shown frames per second, the time a frame\n"
                          "takes and where it goes (audio, picture, title, upload, swap), late frames, the audio queue, the\n"
                          "picture's rendered and shown sizes and, on the Switch, the CPU clock.\n"
                          "FPS only: just the frame rate.  OFF: nothing drawn.\n"
                          "It only reads clocks; it changes nothing in the game.");
      ImGui::Separator();
#ifdef CT_NO_GL
      ImGui::TextDisabled("Video backend: SDL_Renderer (the only one in this build).");
#else
      const char* bk[] = { "SDL (Direct3D / automatic)", "OpenGL 3.3", "Vulkan" };
      if (ImGui::Combo("Video backend", &s->backend, bk, 3)) ch |= CT_OVL_RESTART;
      if (s->backend == CT_BACKEND_SDL) {
        const char* drv[] = { "Automatic", "Direct3D 9", "Direct3D 11", "OpenGL (SDL's)", "Software" };
        const char* drvKey[] = { "", "direct3d", "direct3d11", "opengl", "software" };
        int cur = 0;
        for (int i = 1; i < 5; i++) if (!strcmp(s->renderDriver, drvKey[i])) cur = i;
        if (ImGui::Combo("SDL driver", &cur, drv, 5)) {
          snprintf(s->renderDriver, sizeof s->renderDriver, "%s", drvKey[cur]);
          ch |= CT_OVL_RESTART;
        }
      }
#endif
      ImGui::TextDisabled("In use: %s", g_backendName);
#ifndef CT_NO_GL
      ImGui::TextDisabled("The backend and the driver take effect the next time the game starts.");
#endif
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Audio", NULL, g_selTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
      ImGui::SliderInt("Volume", &s->volume, 0, 100, "%d%%");
      bool m = s->mute != 0;
      if (ImGui::Checkbox("Mute", &m)) s->mute = m;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Controls", NULL, g_selTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
      ImGui::Text("Controller: %s", g_padName[0] ? g_padName : "none connected");
      bool st = s->padStick != 0;
      if (ImGui::Checkbox("Left stick moves like the d-pad", &st)) s->padStick = st;
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
      ImGui::TextWrapped("Click a button, then press the new controller button; right click clears.");
      ImGui::PopStyleColor();
      if (ImGui::BeginTable("binds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("SNES button", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        for (int b = 0; b < CT_BTN_COUNT; b++) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(CtSettingsButtonName(b));
          ImGui::TableSetColumnIndex(1); bindCell(b, padName(s->pad[b]));
        }
        ImGui::EndTable();
      }
      if (ImGui::Button("Restore defaults")) {
        CtSettings d; CtSettingsDefaults(&d);
        memcpy(s->keys, d.keys, sizeof s->keys); memcpy(s->pad, d.pad, sizeof s->pad); s->padStick = d.padStick;
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Sync", NULL, g_selTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.3f, 1.f));
      ImGui::TextWrapped("%s", "THESE SETTINGS UNTESTED ON THIS PLATFORM. USE AT OWN RISK, PLEASE PROVIDE FEEDBACK.");
      ImGui::PopStyleColor();
      ImGui::Spacing();
      ImGui::RadioButton("Frame-perfect", &s->sync, CT_SYNC_FRAME_PERFECT);
      ImGui::TextWrapped("Reproduces the SNES's timing frame by frame: the pauses while music loads, the load and menu "
                         "waits, the frames the console spent for being slow.");
      ImGui::RadioButton("Free", &s->sync, CT_SYNC_FREE);
      ImGui::TextWrapped("Without the waits that only existed because of the hardware's limits, when they do not change the game.");
      ImGui::Spacing();
      ImGui::TextWrapped("In Free: location loads and the reload after the menu are not black; scripts do not stop while "
                         "music loads; a battle starts and ends without waiting for the sound upload.");
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
      ImGui::TextWrapped("Takes effect at once. The play clock counts fewer frames, as on a faster console.");
      ImGui::PopStyleColor();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    g_selTab = -1;
  }
  ImGui::Separator();
  if (ImGui::Button("Back to the game")) ch |= CT_OVL_CLOSE;
  ImGui::SameLine();
  if (ImGui::Button("Quit the game")) ch |= CT_OVL_QUIT;
  ImGui::End();
  ImGui::Render();
  return ch;
}

// ================= THE SAVE/LOAD STATE MENU (ZL + ZR) =================
// The look and the window of the options, the hands of a touch screen: every control is a large button or a row you can press with a finger, the
// grid fills the window, the text is large. Gamepad navigation works as well (A presses the focused button, B backs out of a popup).
enum { SM_NONE = 0, SM_CONFIRM_ACT, SM_RESTORE_LIST, SM_RESTORE_CONFIRM };
static int g_smSlot;                                        // the selected slot, kept between openings
static char g_smMsg[240]; static int g_smMsgOk; static unsigned g_smSerial = ~0u;
static char g_smStamp[48]; static int g_smHave, g_smStampSlot = -1, g_smStampAge;
static CtSsBackup g_smList[32]; static int g_smN;
static int g_smPopup, g_smPopupOpened, g_smActKind, g_smRestoreIdx = -1, g_smDebugPopup;

extern "C" void CtStateMenuOpen(void) {
  g_smPopup = SM_NONE; g_smPopupOpened = 0; g_smMsg[0] = 0; g_smStampSlot = -1; g_smRestoreIdx = -1;
  g_smSerial = ~0u; int ok; unsigned ser; CtSsMessage(CtSsDefault(), &ok, &ser); g_smSerial = ser;     // (an old result is not this opening's)
}
extern "C" int CtStateMenuPopupOpen(void) { return g_smPopup != SM_NONE; }
extern "C" void CtStateMenuDebugPopup(int which) { g_smDebugPopup = which; }
static void smSay(int ok, const char* fmt, const char* a) { snprintf(g_smMsg, sizeof g_smMsg, fmt, a ? a : ""); g_smMsgOk = ok; }
static void smRefreshStamp(void) {
  g_smHave = CtSsSlotStamp(CtSsDefault(), g_smSlot, g_smStamp, sizeof g_smStamp) == 0;
  g_smStampSlot = g_smSlot; g_smStampAge = 0;
}
// A large button; `disabled` greys it and ignores presses.
static bool smButton(const char* label, ImVec2 sz, bool disabled) {
  if (disabled) ImGui::BeginDisabled();
  bool r = ImGui::Button(label, sz);
  if (disabled) ImGui::EndDisabled();
  return r && !disabled;
}
// A grid box: one or two lines of text, each centred and shrunk to fit the box (a time stamp in a narrow box must not be cut off).
static bool smGridButton(const char* id, const char* line1, const char* line2, ImVec2 sz, bool disabled) {
  bool r = smButton(id, sz, disabled);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const ImU32 col = ImGui::GetColorU32(disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text);
  ImFont* font = ImGui::GetFont();
  const float base = ImGui::GetFontSize(), room = (b.x - a.x) - base;             // (half a character of margin each side)
  const char* lines[2] = { line1, line2 }; float sizes[2], widths[2]; int n = line2 ? 2 : 1; float totalH = 0;
  for (int i = 0; i < n; i++) {
    widths[i] = ImGui::CalcTextSize(lines[i]).x; sizes[i] = base;
    if (widths[i] > room) { sizes[i] = base * room / widths[i]; widths[i] = room; }
    totalH += sizes[i];
  }
  float y = a.y + ((b.y - a.y) - totalH - (n - 1) * base * 0.3f) * 0.5f;
  for (int i = 0; i < n; i++) {
    dl->AddText(font, sizes[i], ImVec2(a.x + ((b.x - a.x) - widths[i]) * 0.5f, y), col, lines[i]);
    y += sizes[i] + base * 0.3f;
  }
  return r;
}
static void smStartPopup(int which) { g_smPopup = which; g_smPopupOpened = 0; }

extern "C" int CtStateMenuFrame(void) {
  if (!g_ok) return CT_SMENU_CLOSE;
  int ret = 0;
  CtSettings* s = CtSettingsGet();
  CtSs* ss = CtSsDefault();
#ifdef CT_HAVE_VULKAN
  if (g_vk) ImGui_ImplVulkan_NewFrame(); else
#endif
#ifndef CT_NO_GL
  if (g_gl) ImGui_ImplOpenGL3_NewFrame(); else
#endif
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  if (g_smDebugPopup) {                                      // (CT_OVERLAY_SHOT: look at a popup)
    if (g_smDebugPopup == 1) { g_smN = CtSsListBackups(ss, g_smList, 32); smStartPopup(SM_RESTORE_LIST); }
    else if (g_smDebugPopup == 2) { g_smActKind = CT_SS_SAVE; smStartPopup(SM_CONFIRM_ACT); }
    else if (g_smDebugPopup == 3) { g_smN = CtSsListBackups(ss, g_smList, 32); g_smRestoreIdx = 0; smStartPopup(SM_RESTORE_CONFIRM); }
    g_smDebugPopup = 0;
  }
  // the message of the last request the host ran, and the time stamp of the selected slot
  { int ok; unsigned ser; const char* m = CtSsMessage(ss, &ok, &ser);
    if (ser != g_smSerial) { g_smSerial = ser; snprintf(g_smMsg, sizeof g_smMsg, "%s", m); g_smMsgOk = ok; g_smStampSlot = -1; } }
  if (g_smStampSlot != g_smSlot || ++g_smStampAge > 30) smRefreshStamp();

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 size(vp->WorkSize.x * 0.8f, vp->WorkSize.y * 0.85f);
  if (size.x < 420) size.x = vp->WorkSize.x; if (size.y < 320) size.y = vp->WorkSize.y;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::Begin("SAVE/LOAD STATE MENU", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
  const float fh = ImGui::GetFontSize();
  ImGui::TextWrapped("Save/Load states from this menu.");
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fh * 0.5f, fh * 0.4f));              // the checkboxes and the list: a finger's size
  bool b = s->statesBackup != 0;
  if (ImGui::Checkbox("backup current state before overwriting?", &b)) s->statesBackup = b;
  bool c = s->statesConfirm != 0;
  if (ImGui::Checkbox("confirm on load/save?", &c)) s->statesConfirm = c;
  { char items[CT_SS_SLOTS][12]; const char* ptr[CT_SS_SLOTS];
    for (int i = 0; i < CT_SS_SLOTS; i++) { snprintf(items[i], sizeof items[i], "Slot %d", i); ptr[i] = items[i]; }
    ImGui::SetNextItemWidth(fh * 12.0f);
    if (ImGui::BeginCombo("##slot", ptr[g_smSlot], ImGuiComboFlags_HeightLarge)) {
      for (int i = 0; i < CT_SS_SLOTS; i++) if (ImGui::Selectable(ptr[i], i == g_smSlot, 0, ImVec2(0, fh * 1.6f))) g_smSlot = i;
      ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::TextUnformatted("save slot"); }
  ImGui::PopStyleVar();
  ImGui::Spacing();

  // the 2x2 grid of touch boxes, sized to fill what is left of the window above the "return to game" button and the message line
  const ImVec2 sp = ImGui::GetStyle().ItemSpacing;
  const float retH = fh * 2.8f, msgH = fh * 1.6f;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  float bw = (avail.x - sp.x) * 0.5f;
  float bh = (avail.y - retH - msgH - sp.y * 3.0f) * 0.5f; if (bh < fh * 3.0f) bh = fh * 3.0f;
  char lab[160];
  ImGui::SetWindowFontScale(1.3f);
  snprintf(lab, sizeof lab, "SAVE STATE (SLOT %d)", g_smSlot);
  if (smGridButton("##save", lab, NULL, ImVec2(bw, bh), false)) { if (s->statesConfirm) { g_smActKind = CT_SS_SAVE; smStartPopup(SM_CONFIRM_ACT); } else { CtSsSetBackup(ss, s->statesBackup); CtSsRequest(ss, CT_SS_SAVE, g_smSlot); ret |= CT_SMENU_SERVICE; } }
  ImGui::SameLine();
  snprintf(lab, sizeof lab, "LOAD STATE (SLOT %d)", g_smSlot);
  if (smGridButton("##load", lab, g_smHave ? g_smStamp : "NO STATE CURRENTLY SAVED", ImVec2(bw, bh), !g_smHave)) { if (s->statesConfirm) { g_smActKind = CT_SS_LOAD; smStartPopup(SM_CONFIRM_ACT); } else { CtSsRequest(ss, CT_SS_LOAD, g_smSlot); ret |= CT_SMENU_SERVICE; } }
  if (smGridButton("##backup", "BACKUP ALL SAVES", NULL, ImVec2(bw, bh), false)) {
    char name[96]; const char* why = NULL;
    if (CtSsBackupAll(ss, name, sizeof name, &why)) smSay(0, "Backup failed: %s", why);
    else { char sh[48]; CtSsBackup tmp[32]; int n = CtSsListBackups(ss, tmp, 32); snprintf(sh, sizeof sh, "%s", name); for (int i = 0; i < n; i++) if (!strcmp(tmp[i].file, name)) snprintf(sh, sizeof sh, "%s", tmp[i].shown);
           smSay(1, "Backed up all saves: %s", sh); }
  }
  ImGui::SameLine();
  if (smGridButton("##restore", "RESTORE ALL SAVES", NULL, ImVec2(bw, bh), false)) {
    g_smN = CtSsListBackups(ss, g_smList, 32); smStartPopup(SM_RESTORE_LIST);
  }
  ImGui::SetWindowFontScale(1.0f);

  if (smButton("Return to game", ImVec2(-1, retH), false)) ret |= CT_SMENU_CLOSE;
  if (g_smMsg[0]) ImGui::TextColored(g_smMsgOk ? ImVec4(0.5f, 1.f, 0.5f, 1.f) : ImVec4(1.f, 0.6f, 0.4f, 1.f), "%s", g_smMsg);

  // ---- popups (modal, centred, with large YES / NO) ----
  const ImVec2 mid(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
  const ImVec2 yn(fh * 9.0f, fh * 2.6f);
  if (g_smPopup != SM_NONE && !g_smPopupOpened) {
    ImGui::OpenPopup(g_smPopup == SM_CONFIRM_ACT ? "Confirm##act" : g_smPopup == SM_RESTORE_LIST ? "Restore all saves##list" : "Confirm##restore");
    g_smPopupOpened = 1;
  }
  ImGui::SetNextWindowPos(mid, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Confirm##act", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    if (g_smActKind == CT_SS_SAVE)
      ImGui::Text(g_smHave ? "Overwrite the state in slot %d?" : "Save to slot %d?", g_smSlot);
    else ImGui::Text("Load the state in slot %d? Progress since it was saved is lost.", g_smSlot);
    ImGui::Spacing();
    ImGui::SetWindowFontScale(1.2f);
    if (ImGui::Button("YES", yn)) {
      if (g_smActKind == CT_SS_SAVE) CtSsSetBackup(ss, s->statesBackup);
      CtSsRequest(ss, g_smActKind, g_smSlot); ret |= CT_SMENU_SERVICE;
      g_smPopup = SM_NONE; ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("NO", yn)) { g_smPopup = SM_NONE; ImGui::CloseCurrentPopup(); }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndPopup();
  }
  ImGui::SetNextWindowPos(mid, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(size.x * 0.8f, size.y * 0.8f), ImGuiCond_Always);
  if (ImGui::BeginPopupModal("Restore all saves##list", NULL, ImGuiWindowFlags_NoMove)) {
    if (g_smN == 0) ImGui::TextWrapped("No backups found. BACKUP ALL SAVES makes one in the backups folder.");
    else ImGui::TextWrapped("Select a backup to restore (newest first):");
    ImGui::Spacing();
    float listH = ImGui::GetContentRegionAvail().y - yn.y - sp.y * 2.0f;
    if (ImGui::BeginChild("##backups", ImVec2(0, listH), ImGuiChildFlags_Borders)) {
      ImGui::SetWindowFontScale(1.2f);
      for (int i = 0; i < g_smN; i++) {
        char row[80]; snprintf(row, sizeof row, "%s##bk%d", g_smList[i].shown, i);
        if (ImGui::Selectable(row, false, 0, ImVec2(0, fh * 2.2f))) { g_smRestoreIdx = i; g_smPopup = SM_NONE; ImGui::CloseCurrentPopup(); smStartPopup(SM_RESTORE_CONFIRM); }
      }
      ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::EndChild();
    if (ImGui::Button("CANCEL", yn)) { g_smPopup = SM_NONE; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
  }
  ImGui::SetNextWindowPos(mid, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Confirm##restore", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    const char* when = (g_smRestoreIdx >= 0 && g_smRestoreIdx < g_smN) ? g_smList[g_smRestoreIdx].shown : "?";
    ImGui::Text("Restore all saves from %s?", when);
    ImGui::TextWrapped("This overwrites the current save states with the ones in that backup.");
    ImGui::Spacing();
    ImGui::SetWindowFontScale(1.2f);
    if (ImGui::Button("YES, RESTORE", yn)) {
      const char* why = NULL;
      if (g_smRestoreIdx >= 0 && g_smRestoreIdx < g_smN && !CtSsRestoreBackup(ss, g_smList[g_smRestoreIdx].file, &why)) smSay(1, "Restored all saves from %s", when);
      else smSay(0, "Restore failed: %s", why ? why : "no backup chosen");
      g_smStampSlot = -1; g_smPopup = SM_NONE; ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("NO", yn)) { g_smPopup = SM_NONE; ImGui::CloseCurrentPopup(); }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndPopup();
  }
  // a popup dismissed by the pad's B / Esc (ImGui closed it itself): the menu is free again
  if (g_smPopup != SM_NONE && g_smPopupOpened && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) g_smPopup = SM_NONE;
  ImGui::End();
  ImGui::Render();
  return ret;
}

extern "C" void CtOverlayRender(void) {
  if (!g_ok) return;
#ifdef CT_HAVE_VULKAN
  if (g_vk) { VkCommandBuffer c = (VkCommandBuffer)CtVkCommandBuffer(g_vk);
             if (c) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), c); return; }
#endif
#ifndef CT_NO_GL
  if (g_gl) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); else
#endif
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
}
