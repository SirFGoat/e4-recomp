// E4 launcher: pick Original or HD Remaster, the render resolution and display
// options, then start e4 with the matching command-line settings.
// SDL3 + Dear ImGui, so the same code builds on Windows and macOS.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <cmath>
#include <map>
#include <random>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "calibration.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Settings {
  bool hd = true;            // HD Remaster vs Original
#ifdef __APPLE__
  // Apple GPUs: scale 3 drops to ~45 fps in gameplay on an M4 Pro, 2 holds 60.
  int resolution_scale = 2;  // internal render = 1280x720 * scale
#else
  int resolution_scale = 3;  // internal render = 1280x720 * scale
#endif
  bool fullscreen = true;
  bool vsync = true;
  int aa = 0;     // 0 none, 1 fxaa, 2 fxaa_extreme
  int aniso = 5;  // 0 off .. 5 = 16x
  int particle_count = 12;
  int glow = 100;  // bloom strength in percent, 0 = off
  std::string game_dir;
  std::string texture_dir;
  // Leaderboard identity: the gamertag shown on the leaderboards and a random
  // player ID made on first run (the profile XUID; keeps your scores yours).
  std::string player_name;
  std::string player_id;
  std::string player_key;  // secret: the leaderboard server ties player_id to it
  // How far the beat bar / beat timing runs behind the game's music clock so it lines up
  // with the music as heard (e4_beat_offset_ms).
  int beat_offset = 0;
};

// Music decoder lead (xma_decode_ahead_ms): decoded audio kept ahead of the game's mixer.
constexpr int kXmaDecodeAheadMs = 10;

// Gamertag rules from the 360: 1-15 letters, digits and spaces.
std::string CleanName(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') out += c;
    if (out.size() == 15) break;
  }
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string NewPlayerKey() {
  std::random_device rd;
  std::string key;
  char buf[9];
  for (int i = 0; i < 8; ++i) {
    std::snprintf(buf, sizeof(buf), "%08x", unsigned(rd()));
    key += buf;
  }
  return key;  // 64 hex characters
}

std::string NewPlayerId() {
  // Online-style XUID: 0x0009 prefix + 48 random bits.
  std::random_device rd;
  uint64_t r = (uint64_t(rd()) << 32) ^ rd();
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                (unsigned long long)(0x0009000000000000ull | (r & 0x0000FFFFFFFFFFFFull)));
  return buf;
}

const char* kAaValues[] = {"none", "fxaa", "fxaa_extreme"};
const char* kAaLabels[] = {"Off", "FXAA", "FXAA (strong)"};
const char* kAnisoLabels[] = {"Off", "1x", "2x", "4x", "8x", "16x"};

struct ResOption {
  int scale;
  const char* label;
};
const ResOption kResolutions[] = {
    {1, "1280 x 720   (original)"},
    {2, "2560 x 1440"},
    {3, "3840 x 2160   (4K)"},
    {4, "5120 x 2880   (5K)"},
    {6, "7680 x 4320   (8K)"},
};

fs::path BaseDir();
fs::path SettingsPath() { return BaseDir() / "e4_launcher.ini"; }

void Load(Settings& s) {
  std::ifstream in(SettingsPath());
  std::string line;
  std::map<std::string, std::string> kv;
  while (std::getline(in, line)) {
    auto eq = line.find('=');
    if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  auto i = [&](const char* k, int& v) {
    if (kv.count(k)) v = std::atoi(kv[k].c_str());
  };
  auto b = [&](const char* k, bool& v) {
    if (kv.count(k)) v = kv[k] == "1";
  };
  b("hd", s.hd);
  i("resolution_scale", s.resolution_scale);
  b("fullscreen", s.fullscreen);
  b("vsync", s.vsync);
  i("aa", s.aa);
  i("aniso", s.aniso);
  i("particle_count", s.particle_count);
  i("glow", s.glow);
  s.glow = std::clamp(s.glow, 0, 150);
  // "beat_fine_ms": fine-tuning on top of the game's half-beat shift. The older
  // "beat_offset" / "beat_offset_ms" keys held whole-offset estimates, so they are not read.
  i("beat_fine_ms", s.beat_offset);
  s.beat_offset = std::clamp(s.beat_offset, -150, 150);
  if (kv.count("game_dir")) s.game_dir = kv["game_dir"];
  if (kv.count("texture_dir")) s.texture_dir = kv["texture_dir"];
  if (kv.count("player_name")) s.player_name = CleanName(kv["player_name"]);
  if (kv.count("player_id") && kv["player_id"].size() == 16) s.player_id = kv["player_id"];
  if (s.player_id.empty()) s.player_id = NewPlayerId();
  if (kv.count("player_key") && kv["player_key"].size() >= 32) s.player_key = kv["player_key"];
  if (s.player_key.empty()) s.player_key = NewPlayerKey();
  s.aa = std::clamp(s.aa, 0, 2);
  s.aniso = std::clamp(s.aniso, 0, 5);
  s.particle_count = std::clamp(s.particle_count, 0, 32);
  s.resolution_scale = std::clamp(s.resolution_scale, 1, 8);
}

void Save(const Settings& s) {
  std::ofstream out(SettingsPath());
  out << "hd=" << (s.hd ? 1 : 0) << "\n"
      << "resolution_scale=" << s.resolution_scale << "\n"
      << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n"
      << "vsync=" << (s.vsync ? 1 : 0) << "\n"
      << "aa=" << s.aa << "\n"
      << "aniso=" << s.aniso << "\n"
      << "particle_count=" << s.particle_count << "\n"
      << "glow=" << s.glow << "\n"
      << "game_dir=" << s.game_dir << "\n"
      << "texture_dir=" << s.texture_dir << "\n"
      << "player_name=" << s.player_name << "\n"
      << "player_id=" << s.player_id << "\n"
      << "player_key=" << s.player_key << "\n"
      << "beat_fine_ms=" << s.beat_offset << "\n";
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::string U8(const fs::path& p) {
  auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path FromU8(const std::string& s) {
  return fs::path(std::u8string(s.begin(), s.end()));
}

fs::path BaseDir() {
  // The SDK's SDL build leaves out SDL_filesystem, so ask the OS directly.
  std::error_code ec;
#ifdef _WIN32
  std::wstring buf(32768, L'\0');
  DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
  if (n > 0 && n < buf.size()) return fs::path(buf.substr(0, n)).parent_path();
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) return fs::weakly_canonical(buf, ec).parent_path();
#else
  auto p = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) return p.parent_path();
#endif
  return fs::current_path(ec);
}

fs::path GameExe() {
#ifdef _WIN32
  return BaseDir() / "e4.exe";
#else
  return BaseDir() / "e4";
#endif
}

bool IsGameDir(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p / "default.xex", ec);
}

// Look for a folder called `name` next to the launcher or in any parent folder.
std::optional<fs::path> FindUp(const char* name, bool (*valid)(const fs::path&)) {
  fs::path dir = BaseDir();
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    fs::path c = dir / name;
    std::error_code ec;
    if (fs::is_directory(c, ec) && (!valid || valid(c))) return fs::weakly_canonical(c, ec);
    fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return std::nullopt;
}

void AutoDetect(Settings& s) {
  if (s.game_dir.empty() || !IsGameDir(FromU8(s.game_dir))) {
    if (auto p = FindUp("game", IsGameDir)) s.game_dir = U8(*p);
  }
  std::error_code ec;
  if (s.texture_dir.empty() || !fs::is_directory(FromU8(s.texture_dir), ec)) {
    if (auto p = FindUp("textures", nullptr)) s.texture_dir = U8(*p);
  }
}

// ---------------------------------------------------------------------------
// Folder picker (SDL calls back on another thread on some platforms)
// ---------------------------------------------------------------------------

std::mutex g_pick_mutex;
std::string* g_pick_target = nullptr;
std::optional<std::string> g_pick_result;

void SDLCALL OnFolderPicked(void*, const char* const* list, int) {
  std::lock_guard<std::mutex> lock(g_pick_mutex);
  if (list && list[0]) g_pick_result = std::string(list[0]);
}

void PickFolder(SDL_Window* window, std::string& target) {
  {
    std::lock_guard<std::mutex> lock(g_pick_mutex);
    g_pick_target = &target;
    g_pick_result.reset();
  }
  SDL_ShowOpenFolderDialog(OnFolderPicked, nullptr, window,
                           target.empty() ? nullptr : target.c_str(), false);
}

void ApplyPickedFolder() {
  std::lock_guard<std::mutex> lock(g_pick_mutex);
  if (g_pick_result && g_pick_target) {
    *g_pick_target = *g_pick_result;
    g_pick_result.reset();
    g_pick_target = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Launch
// ---------------------------------------------------------------------------

std::vector<std::string> BuildArgs(const Settings& s) {
  auto flag = [](const char* name, const std::string& v) { return std::string("--") + name + "=" + v; };
  auto bstr = [](bool b) { return std::string(b ? "true" : "false"); };
  std::vector<std::string> a;
  a.push_back(U8(GameExe()));
  a.push_back(flag("game_data_root", s.game_dir));
  a.push_back(flag("gpu_plugin", "xenos"));
  a.push_back(flag("license_mask", "1"));
  a.push_back(flag("resolution_scale", std::to_string(s.resolution_scale)));
  a.push_back(flag("anisotropic_override", std::to_string(s.aniso)));
  a.push_back(flag("swap_post_effect", kAaValues[s.aa]));
  a.push_back(flag("fullscreen", bstr(s.fullscreen)));
  a.push_back(flag("vsync", bstr(s.vsync)));
  a.push_back(flag("texture_dump", "false"));
  // HD Remaster = upscaled textures + extra particles + "Quit" menu text.
  a.push_back(flag("texture_replace", bstr(s.hd && !s.texture_dir.empty())));
  if (!s.texture_dir.empty()) a.push_back(flag("texture_dir", s.texture_dir));
  a.push_back(flag("e4_extra_particles", bstr(s.hd && s.particle_count > 0)));
  a.push_back(flag("e4_particle_count", std::to_string(s.particle_count)));
  a.push_back(flag("e4_text_patches", bstr(s.hd)));
  // Neon glow baked into enemy / explosion / title sprites (replace/@glow). The
  // full-screen bloom stays off: it can't tell sprites from HUD and text.
  bool glow = s.hd && s.glow > 0;
  a.push_back(flag("bloom", "false"));
  a.push_back(flag("texture_variants", glow ? "glow" : ""));
  a.push_back(flag("e4_sprite_glow", bstr(glow)));
  a.push_back(flag("user_name", s.player_name));
  a.push_back(flag("user_xuid", s.player_id));
  a.push_back(flag("e4_player_token", s.player_key));
  // The game only opens its leaderboard menus when signed in to Xbox LIVE.
  a.push_back(flag("user_signin_live", "true"));
  // 2 queued audio frames (~11 ms) instead of the engine's 8 (~43 ms): the sound comes
  // out as soon after the game's beat as the audio device allows.
  a.push_back(flag("audio_maxqframes", "2"));
  // Music decoded only ~10 ms ahead of the game's mixer (the engine default fills the
  // decoder's whole output ring, ~130 ms for the music layers).
  a.push_back(flag("xma_decode_ahead_ms", std::to_string(kXmaDecodeAheadMs)));
  a.push_back(flag("e4_beat_offset_ms", std::to_string(s.beat_offset)));
  // Tap monitor: record the game's beat, the music as heard and controller taps
  // (sync_beat.csv / sync_audio.csv / sync_taps.csv next to e4) for checking the sync.
  a.push_back(flag("sync_record", "true"));
  a.push_back(flag("log_file", U8(BaseDir() / "e4_last_run.log")));
  return a;
}

SDL_Process* Launch(const Settings& s, std::string& error) {
  std::vector<std::string> args = BuildArgs(s);
  std::vector<const char*> argv;
  for (auto& x : args) argv.push_back(x.c_str());
  argv.push_back(nullptr);

  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
  std::string wd = U8(BaseDir());
  SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, wd.c_str());
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
  SDL_Process* proc = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (!proc) {
    error = std::string("Could not start the game: ") + SDL_GetError();
  }
  return proc;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void ApplyStyle(float scale) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark(&st);
  st.WindowRounding = 0.0f;
  st.FrameRounding = 6.0f;
  st.GrabRounding = 6.0f;
  st.ChildRounding = 8.0f;
  st.PopupRounding = 6.0f;
  st.FramePadding = ImVec2(10, 7);
  st.ItemSpacing = ImVec2(10, 10);
  st.WindowPadding = ImVec2(22, 18);
  ImVec4* c = st.Colors;
  const ImVec4 accent(0.98f, 0.30f, 0.55f, 1.0f);  // E4 magenta
  c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.09f, 1.0f);
  c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.21f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.20f, 0.29f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.24f, 0.35f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.25f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.25f, 0.36f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.30f, 0.44f, 1.0f);
  c[ImGuiCol_CheckMark] = accent;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.45f, 0.66f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.22f, 0.20f, 0.30f, 1.0f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.26f, 0.40f, 1.0f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.30f, 0.48f, 1.0f);
  c[ImGuiCol_Separator] = ImVec4(0.22f, 0.22f, 0.30f, 1.0f);
  c[ImGuiCol_Text] = ImVec4(0.93f, 0.93f, 0.97f, 1.0f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.65f, 1.0f);
  st.ScaleAllSizes(scale);
  st.FontScaleDpi = scale;
}

void SectionTitle(const char* text) {
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.66f, 1.0f));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}

// A big selectable card for the version choice.
bool VersionCard(const char* id, const char* title, const char* desc, bool selected, float width) {
  ImGui::PushID(id);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float h = ImGui::GetTextLineHeight() * 4.4f;
  bool clicked = ImGui::InvisibleButton("card", ImVec2(width, h));
  bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bg = ImGui::GetColorU32(selected ? ImVec4(0.24f, 0.12f, 0.22f, 1.0f)
                                         : hovered ? ImVec4(0.16f, 0.16f, 0.22f, 1.0f)
                                                   : ImVec4(0.11f, 0.11f, 0.15f, 1.0f));
  ImU32 border = ImGui::GetColorU32(selected ? ImVec4(0.98f, 0.30f, 0.55f, 1.0f)
                                             : ImVec4(0.24f, 0.24f, 0.32f, 1.0f));
  float r = ImGui::GetStyle().FrameRounding * 1.4f;
  dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), bg, r);
  dl->AddRect(pos, ImVec2(pos.x + width, pos.y + h), border, r, 0, selected ? 2.5f : 1.0f);
  float pad = ImGui::GetStyle().FramePadding.x * 1.4f;
  float lh = ImGui::GetTextLineHeight();
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.15f, ImVec2(pos.x + pad, pos.y + lh * 0.55f),
              ImGui::GetColorU32(ImGuiCol_Text), title);
  ImGui::PushClipRect(pos, ImVec2(pos.x + width - pad, pos.y + h), true);
  dl->AddText(ImVec2(pos.x + pad, pos.y + lh * 1.95f), ImGui::GetColorU32(ImGuiCol_TextDisabled), desc);
  ImGui::PopClipRect();
  ImGui::PopID();
  return clicked;
}

void PathRow(SDL_Window* window, const char* label, std::string& value, bool ok, const char* hint) {
  ImGui::PushID(label);
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  if (ok) {
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "found");
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", hint);
  }
  float browse_w = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "%s", value.c_str());
  if (ImGui::InputText("##path", buf, sizeof(buf))) value = buf;
  ImGui::SameLine();
  if (ImGui::Button("Browse...")) PickFolder(window, value);
  ImGui::PopID();
}

}  // namespace

int main(int argc, char** argv) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "E4 Launcher", SDL_GetError(), nullptr);
    return 1;
  }

  SDL_DisplayID display = SDL_GetPrimaryDisplay();
  float scale = SDL_GetDisplayContentScale(display);
  if (scale <= 0.0f) scale = 1.0f;

  // The SDK's SDL build has no 2D renderer, so draw with SDL_GPU
  // (D3D12/Vulkan on Windows, Metal on macOS).
  // Tall enough for every section, but never taller than the screen (minus the taskbar
  // and title bar); the content scrolls if it doesn't fit.
  int win_h = int(1060 * scale);
  SDL_Rect usable;
  if (SDL_GetDisplayUsableBounds(display, &usable) && usable.h > 0) {
    win_h = std::min(win_h, usable.h - int(48 * scale));
  }
  SDL_Window* window = SDL_CreateWindow("E4 Launcher", int(600 * scale), win_h, 0);
  SDL_GPUDevice* gpu = window ? SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
                                                        SDL_GPU_SHADERFORMAT_METALLIB,
                                                    false, nullptr)
                              : nullptr;
  if (!gpu || !SDL_ClaimWindowForGPUDevice(gpu, window)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "E4 Launcher", SDL_GetError(), window);
    return 1;
  }
  SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

  // A proper UI font when the system has one; ImGui's built-in font otherwise.
  const char* fonts[] = {
#ifdef _WIN32
      "C:\\Windows\\Fonts\\segoeui.ttf",
#elif defined(__APPLE__)
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
#endif
      nullptr};
  bool font_loaded = false;
  for (const char** f = fonts; *f && !font_loaded; ++f) {
    std::error_code ec;
    if (fs::exists(*f, ec)) font_loaded = io.Fonts->AddFontFromFileTTF(*f, 18.0f) != nullptr;
  }
  if (!font_loaded) io.Fonts->AddFontDefault();

  ApplyStyle(scale);
  ImGui_ImplSDL3_InitForSDLGPU(window);
  ImGui_ImplSDLGPU3_InitInfo init_info;
  init_info.Device = gpu;
  init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu, window);
  init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  ImGui_ImplSDLGPU3_Init(&init_info);

  Settings s;
  Load(s);
  AutoDetect(s);

  std::string error;
  Calibration calib;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--calibrate") calib.Open();
    if (std::string(argv[i]) == "--calibrate-demo") calib.Demo(40.0, 15.0);
  }
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      calib.OnEvent(e);
      ImGui_ImplSDL3_ProcessEvent(&e);
      if (e.type == SDL_EVENT_QUIT) running = false;
      if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window))
        running = false;
    }
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(30);
      continue;
    }
    ApplyPickedFolder();

    // The screen the launcher is on (the game opens on the same one), and the
    // smallest render scale that covers it.
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
    int disp_w = mode ? mode->w : 0, disp_h = mode ? mode->h : 0;
    int native_scale = disp_h > 0 ? std::max(1, (disp_h + 719) / 720) : 0;

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Header
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.9f);
    ImGui::TextUnformatted("E4");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.15f));
    ImGui::TextUnformatted("Every Extend Extra Extreme");
    ImGui::TextDisabled("PC recompilation");
    ImGui::EndGroup();
    ImGui::Separator();

    if (calib.active()) {
      // Beat calibration replaces the settings until it is closed.
      const SDL_DisplayMode* cur = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
      if (calib.Draw(s.beat_offset, cur ? cur->refresh_rate : 60.0f)) Save(s);
    } else {

    // Settings scroll; the PLAY button (and its message line) stays pinned below them.
    const float btn_h = ImGui::GetTextLineHeight() * 2.6f;
    const float footer_h = btn_h + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##settings", ImVec2(0, -footer_h));
    const float full_w = ImGui::GetContentRegionAvail().x;
    const float label_w = full_w * 0.36f;
    auto row = [&](const char* label) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      ImGui::SameLine(label_w);
      ImGui::SetNextItemWidth(-FLT_MIN);
    };

    // Player
    SectionTitle("PLAYER");
    row("Name");
    {
      char name_buf[16];
      std::snprintf(name_buf, sizeof(name_buf), "%s", s.player_name.c_str());
      if (ImGui::InputTextWithHint("##name", "your name on the leaderboards", name_buf,
                                   sizeof(name_buf))) {
        s.player_name = CleanName(name_buf);
      }
    }

    // Version
    SectionTitle("VERSION");
    float card_w = (full_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (VersionCard("hd", "HD Remaster", "AI-upscaled art, glow,\nexplosion particles", s.hd, card_w)) s.hd = true;
    ImGui::SameLine();
    if (VersionCard("orig", "Original", "As released on\nXbox 360", !s.hd, card_w)) s.hd = false;

    // Display
    SectionTitle("DISPLAY");
    row("Resolution");
    std::string current = "Custom";
    for (auto& r : kResolutions)
      if (r.scale == s.resolution_scale) current = r.label;
    if (ImGui::BeginCombo("##res", current.c_str())) {
      for (auto& r : kResolutions) {
        std::string label = r.label;
        if (r.scale == native_scale) label += "   - matches your screen";
        if (ImGui::Selectable(label.c_str(), r.scale == s.resolution_scale)) s.resolution_scale = r.scale;
      }
      ImGui::EndCombo();
    }
    if (disp_w > 0) {
      ImGui::SetCursorPosX(label_w);
      if (s.resolution_scale > native_scale && native_scale > 0)
        ImGui::TextDisabled("Supersampled on your %d x %d screen", disp_w, disp_h);
      else
        ImGui::TextDisabled("Your screen: %d x %d", disp_w, disp_h);
    }

    row("Window");
    int win = s.fullscreen ? 0 : 1;
    if (ImGui::Combo("##win", &win, "Fullscreen\0Windowed\0")) s.fullscreen = win == 0;
    row("Anti-aliasing");
    ImGui::Combo("##aa", &s.aa, kAaLabels, IM_ARRAYSIZE(kAaLabels));
    row("Texture filtering");
    ImGui::Combo("##aniso", &s.aniso, kAnisoLabels, IM_ARRAYSIZE(kAnisoLabels));
    row("V-Sync");
    ImGui::Checkbox("##vsync", &s.vsync);

    // HD options
    SectionTitle("HD REMASTER");
    ImGui::BeginDisabled(!s.hd);
    row("Explosion sparks");
    ImGui::SliderInt("##sparks", &s.particle_count, 0, 32, s.particle_count == 0 ? "Off" : "%d per explosion");
    row("Neon glow");
    bool glow_on = s.glow > 0;
    if (ImGui::Checkbox("##glow", &glow_on)) s.glow = glow_on ? 100 : 0;
    ImGui::SameLine();
    ImGui::TextDisabled("enemies, explosions, title");
    ImGui::EndDisabled();
    if (!s.hd) ImGui::TextDisabled("Select HD Remaster to use these.");

    // Beat timing
    SectionTitle("BEAT TIMING");
    row("Beat fine-tune");
    ImGui::SliderInt("##beat", &s.beat_offset, -150, 150, "%+d ms");
    ImGui::SetCursorPosX(label_w);
    ImGui::TextDisabled("Fine-tunes the beat bar and vibration against the music (0 = default).");
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Button("Calibrate...")) calib.Open();
    ImGui::SameLine();
    ImGui::TextDisabled("tap along to a click and a ball");

    // Paths
    SectionTitle("FILES");
    std::error_code ec;
    PathRow(window, "Game folder", s.game_dir, IsGameDir(FromU8(s.game_dir)),
            "pick the folder with default.xex");
    PathRow(window, "HD textures", s.texture_dir, fs::is_directory(FromU8(s.texture_dir) / "replace", ec),
            "pick the folder with 'replace'");

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Play
    const float play_w = ImGui::GetContentRegionAvail().x;
    bool exe_ok = fs::exists(GameExe(), ec);
    bool can_play = exe_ok && IsGameDir(FromU8(s.game_dir)) && !s.player_name.empty();
    if (error.empty() && exe_ok && !s.player_name.empty()) {
      ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    }
    if (!exe_ok) {
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "e4 not found next to the launcher.");
    } else if (s.player_name.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Enter a player name first.");
    } else if (!error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", error.c_str());
    }
    ImGui::BeginDisabled(!can_play);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.22f, 0.47f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.32f, 0.57f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.18f, 0.40f, 1.0f));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    if (ImGui::Button("PLAY", ImVec2(play_w, btn_h))) {
      Save(s);
      if (SDL_Process* proc = Launch(s, error)) {
        SDL_DestroyProcess(proc);  // detached; the game keeps running
        running = false;
      }
    }
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    }  // settings
    ImGui::End();

    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUTexture* swapchain = nullptr;
    if (cmd && SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, nullptr, nullptr) && swapchain) {
      ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);
      SDL_GPUColorTargetInfo target = {};
      target.texture = swapchain;
      target.clear_color = SDL_FColor{0.06f, 0.06f, 0.09f, 1.0f};
      target.load_op = SDL_GPU_LOADOP_CLEAR;
      target.store_op = SDL_GPU_STOREOP_STORE;
      SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
      ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
      SDL_EndGPURenderPass(pass);
    }
    if (cmd) SDL_SubmitGPUCommandBuffer(cmd);
  }

  Save(s);
  SDL_WaitForGPUIdle(gpu);
  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();
  SDL_ReleaseWindowFromGPUDevice(gpu, window);
  SDL_DestroyGPUDevice(gpu);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
