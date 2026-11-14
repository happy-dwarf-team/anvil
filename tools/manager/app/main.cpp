// AnvilManager.exe (design §15, issue #59): the player's installer and
// management tool. One exe with the Anvil package of its own version in its
// resources; Dear ImGui on Direct3D 11.
//
//   AnvilManager.exe [--game <EU4 folder>]
//   AnvilManager.exe --check-package          exit code 0 when the package and the catalog it carries are sound
//   AnvilManager.exe --remove-after-exit <exe> <pid>   (internal: the uninstall's last step)
//   AnvilManager.exe --screenshot <dir> --game <folder> [--lang <code>] [--scale <percent>]
//                    [--tab <name>] [--select <plugin>] [--build <id>] [--dialog install|uninstall]
//       ADR 0013, point 13: draws each tab (or only <name>) in a hidden window,
//       writes <dir>/<tab>.png and exits; it reads the folder and writes nothing
//       else. --build takes a fixture's eu4.exe for that game build.
#include "core/log.hpp"
#include "core/playset.hpp"
#include "i18n/cjk_font.hpp"
#include "i18n/translate.hpp"
#include "manager_app.hpp"
#include "platform.hpp"
#include "ui.hpp"
#include "ui/imgui_colors.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {

using namespace anvil::manager;
namespace fs = std::filesystem;

struct Graphics {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    bool occluded = false;
    UINT resize_width = 0;
    UINT resize_height = 0;

    bool create(HWND window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level{};
        HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                                       D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
        if (FAILED(result)) {  // no GPU driver (a CI runner, a VM): the software rasterizer
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                                   D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
        }
        if (FAILED(result)) return false;
        create_target();
        return true;
    }
    void create_target() {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
            device->CreateRenderTargetView(back, nullptr, &target);
            back->Release();
        }
    }
    void release_target() {
        if (target != nullptr) target->Release();
        target = nullptr;
    }
    ~Graphics() {
        release_target();
        if (swap_chain != nullptr) swap_chain->Release();
        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
    }
};

Graphics* g_graphics = nullptr;
ManagerApp* g_app = nullptr;
float g_scale = 1.0f;
bool g_fonts_dirty = true;
anvil::i18n::Language g_fonts_language = anvil::i18n::Language::English;  // the atlas's

// A font of Windows mapped read-only for the process, as the overlay maps
// them: the atlas reads its glyphs from the view, and the file is not copied
// into memory. Null when it cannot be opened.
struct MappedFont {
    const void* view = nullptr;
    int size = 0;
};

MappedFont map_system_font(const fs::path& path) {
    static std::vector<std::pair<fs::path, MappedFont>> mapped;
    for (const auto& [p, font] : mapped) {
        if (p == path) return font;
    }
    MappedFont font;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < INT_MAX) {
            if (const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
                font.view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
                if (font.view != nullptr) font.size = static_cast<int>(size.QuadPart);
                CloseHandle(mapping);
            }
        }
        CloseHandle(file);
    }
    mapped.emplace_back(path, font);
    return font;
}

void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const fs::path fonts = fs::path(windows) / "Fonts";
    // Chinese, Japanese and Korean: the language's font, merged into each Latin
    // one (i18n/cjk_font.hpp). ImGui rasterizes a glyph when a text first needs it.
    g_fonts_language = anvil::i18n::language();
    const anvil::i18n::FontData cjk = anvil::i18n::cjk_font(g_fonts_language);
    // Plugins' translations and the catalog's (design §5.4) use characters
    // Anvil's subset lacks: the language's font of Windows covers them.
    const wchar_t* system = nullptr;
    switch (g_fonts_language) {
    case anvil::i18n::Language::SimplifiedChinese: system = L"msyh.ttc"; break;
    case anvil::i18n::Language::TraditionalChinese: system = L"msjh.ttc"; break;
    case anvil::i18n::Language::Japanese: system = L"YuGothR.ttc"; break;
    case anvil::i18n::Language::Korean: system = L"malgun.ttf"; break;
    default: break;
    }
    const MappedFont mapped = cjk.size > 0 && system != nullptr ? map_system_font(fonts / system) : MappedFont{};
    // A Latin font of Windows at `size`, the CJK ones merged into it; null when the file is missing.
    const auto add = [&](const wchar_t* file, float size) -> ImFont* {
        const fs::path path = fonts / file;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return nullptr;
        ImFontConfig config;
        config.OversampleH = 2;
        const auto name = path.u8string();  // ImGui opens it as UTF-8
        ImFont* font = io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(name.c_str()), size, &config);
        if (font == nullptr || cjk.size == 0) return font;
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontDataOwnedByAtlas = false;  // the exe's resource
        merge.OversampleH = 1;
        merge.PixelSnapH = true;
        // Source Han Sans fills more of its height with its ascent and
        // descent than Segoe UI, so the same size draws smaller ideographs.
        merge.GlyphOffset.y = 1.0f * scale;
        io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(cjk.data), static_cast<int>(cjk.size), size * 1.12f, &merge);
        if (mapped.view != nullptr) {
            ImFontConfig windows_font = merge;
            windows_font.GlyphOffset.y = 0.0f;
            io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(mapped.view), mapped.size, size, &windows_font);
        }
        return font;
    };
    if (add(L"segoeui.ttf", 17.0f * scale) == nullptr) {
        ImFontConfig fallback;
        fallback.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&fallback);
    }
    // Page titles (docs/ui-design-system.md, "Shell"): Segoe UI Semibold, or the text's font.
    anvil::ui::g_title_font = add(L"seguisb.ttf", 17.0f * 1.18f * scale);
    // Logs and crash reports: Consolas, or the text's font.
    anvil::ui::g_mono_font = add(L"consola.ttf", 15.0f * scale);
    anvil::ui::use_anvil_style();
    ImGui::GetStyle().ScaleAllSizes(scale);
}

bool is_button_message(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

// Posted mouse input (issue #106): automated tests post WM_MOUSEMOVE and the
// button messages to the window without moving the cursor. The Win32 backend
// would lose such a position at once: its TrackMouseEvent sends WM_MOUSELEAVE
// as soon as the real cursor is outside the window, and without tracking it
// reads GetCursorPos every frame while the window is in front. A posted
// message shows itself by a cursor position (GetMessagePos) that is not the
// one in lParam; while the last mouse message was posted, the leave is kept
// from ImGui, so the position stays until the real mouse comes back.
bool g_posted_pointer = false;

bool is_posted(HWND window, LPARAM lparam) {
    POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ClientToScreen(window, &at);
    const DWORD cursor = GetMessagePos();
    return at.x != GET_X_LPARAM(cursor) || at.y != GET_Y_LPARAM(cursor);
}

// Returns true when the message is not for ImGui.
bool filter_mouse(HWND window, UINT message, LPARAM lparam) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    if (message == WM_MOUSELEAVE) return g_posted_pointer;
    if (message != WM_MOUSEMOVE && !is_button_message(message)) return false;
    const bool posted = is_posted(window, lparam);
    if (g_posted_pointer && !posted) {
        // The real mouse is back. The posted move's tracking ended in the leave
        // ImGui did not get, so its leaving is tracked again here.
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
    }
    g_posted_pointer = posted;
    // A button message carries the position of the click; ImGui gets it before
    // the button, so a click lands there even without a move before it. A real
    // click has the position of its last move anyway.
    if (is_button_message(message)) {
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
    }
    return false;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (filter_mouse(window, message, lparam)) return 0;
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return 1;
    switch (message) {
    case WM_SIZE:
        if (g_graphics != nullptr && wparam != SIZE_MINIMIZED) {
            g_graphics->resize_width = LOWORD(lparam);
            g_graphics->resize_height = HIWORD(lparam);
        }
        return 0;
    case WM_DPICHANGED: {
        g_scale = HIWORD(wparam) / 96.0f;
        g_fonts_dirty = true;
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {static_cast<LONG>(720 * g_scale), static_cast<LONG>(480 * g_scale)};
        return 0;
    }
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wparam);
        wchar_t path[MAX_PATH * 4] = {};
        if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) > 0 && DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))) > 0 &&
            g_app != nullptr) {
            g_app->dropped(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;  // no menu on Alt
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// --check-package: the package is in the resources, reads back and carries
// this Manager's version.dll, and the plugin catalog validates (the CI and
// release check).
int check_package() {
    std::string error;
    if (!load_embedded_catalog(error)) return 1;
    const auto package = load_embedded_package(error);
    if (!package) return 1;
    const ZipEntry* dll = package->zip.find("version.dll");
    if (dll == nullptr) return 1;
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    const fs::path copy = fs::path(temp) / (L"anvil-manager-check-" + std::to_wstring(GetCurrentProcessId()) + L".dll");
    const auto bytes = package->zip.read(*dll, error);
    bool ok = bytes.has_value();
    if (ok) {
        HANDLE file = CreateFileW(copy.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes->data(), static_cast<DWORD>(bytes->size()), &written, nullptr) &&
             written == bytes->size();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    const auto product = ok ? read_dll_product(copy) : std::nullopt;
    DeleteFileW(copy.c_str());
    return product && product->name == "Anvil" && product->version == ANVIL_VERSION ? 0 : 1;
}

// The back buffer as a PNG, through WIC; false when any step fails.
bool write_png(Graphics& graphics, const fs::path& file) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(graphics.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(graphics.device->CreateTexture2D(&desc, nullptr, &staging));
    std::vector<uint8_t> pixels;
    if (ok) {
        graphics.context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            // RGBA to opaque BGRA, the PNG encoder's own format.
            pixels.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                uint8_t* out = pixels.data() + static_cast<size_t>(y) * desc.Width * 4;
                for (UINT x = 0; x < desc.Width; ++x) {
                    out[x * 4 + 0] = row[x * 4 + 2];
                    out[x * 4 + 1] = row[x * 4 + 1];
                    out[x * 4 + 2] = row[x * 4 + 0];
                    out[x * 4 + 3] = 255;
                }
            }
            graphics.context->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
    if (!ok) return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(desc.Width, desc.Height)) &&
         SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA &&
         SUCCEEDED(frame->WritePixels(desc.Height, desc.Width * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (frame != nullptr) frame->Release();
    if (encoder != nullptr) encoder->Release();
    if (stream != nullptr) stream->Release();
    if (factory != nullptr) factory->Release();
    return ok;
}

// The screenshot mode's options (the top of this file).
struct ShotOptions {
    fs::path out;
    std::string tab;     // English name; "" for every tab
    std::string select;  // plugin id
    std::string build;
    std::string dialog;  // "install" or "uninstall": the shot shows that dialog over the page
    int scale = 100;
    int width = 1040;  // the window's width at 100 %: a narrower one shows the sidebar's rail
};

// The system's caption in Forge's colours (docs/ui-design-system.md, "Shell"):
// its dark mode on Windows 10 20H1 and later, and on Windows 11 22000 and
// later the caption in the client's own background, so the title bar and the
// window read as one surface. Older builds ignore the attributes they lack.
void style_caption(HWND window) {
    constexpr DWORD kDarkMode = 20, kCorners = 33, kBorder = 34, kCaption = 35, kText = 36;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, kDarkMode, &dark, sizeof(dark));
    const COLORREF caption = RGB(0x12, 0x11, 0x10);  // Forge `window`, the client's own background
    const COLORREF text = RGB(0xec, 0xe8, 0xe1);     // Forge `text`
    const COLORREF border = RGB(0x2c, 0x29, 0x26);   // Forge `line`
    const int round = 2;                             // DWMWCP_ROUND
    DwmSetWindowAttribute(window, kCaption, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, kText, &text, sizeof(text));
    DwmSetWindowAttribute(window, kBorder, &border, sizeof(border));
    DwmSetWindowAttribute(window, kCorners, &round, sizeof(round));
}

// The tabs in their order, by the English names their ids use.
constexpr const char* kShotTabs[] = {"Anvil", "Catalog", "Plugins", "Benchmark", "Crashes", "Log", "About"};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Never load DLLs from the game folder (it holds Anvil's own version.dll) or the current one.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv, argv + argc);
    LocalFree(argv);
    fs::path game;
    std::optional<ShotOptions> shot;
    const auto shot_options = [&]() -> ShotOptions& {
        if (!shot) shot.emplace();
        return *shot;
    };
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--check-package") return check_package();
        if (args[i] == L"--remove-after-exit" && i + 2 < args.size()) {
            return run_remove_after_exit(args[i + 1], static_cast<DWORD>(std::wcstoul(args[i + 2].c_str(), nullptr, 10)));
        }
        if (args[i] == L"--game" && i + 1 < args.size()) game = args[++i];
        if (args[i] == L"--screenshot" && i + 1 < args.size()) shot_options().out = args[++i];
        if (args[i] == L"--tab" && i + 1 < args.size()) shot_options().tab = anvil::narrow(args[++i]);
        if (args[i] == L"--select" && i + 1 < args.size()) shot_options().select = anvil::narrow(args[++i]);
        if (args[i] == L"--build" && i + 1 < args.size()) shot_options().build = anvil::narrow(args[++i]);
        if (args[i] == L"--dialog" && i + 1 < args.size()) shot_options().dialog = anvil::narrow(args[++i]);
        if (args[i] == L"--scale" && i + 1 < args.size()) shot_options().scale = std::max(50, _wtoi(args[++i].c_str()));
        if (args[i] == L"--width" && i + 1 < args.size()) shot_options().width = std::max(720, _wtoi(args[++i].c_str()));
        // The language goes through ANVIL_LANGUAGE, which every later choice of it honours.
        if (args[i] == L"--lang" && i + 1 < args.size()) SetEnvironmentVariableW(L"ANVIL_LANGUAGE", args[++i].c_str());
    }
    if (shot && shot->out.empty()) return 2;
    if (!shot) remove_stale_copies();
    // The language of EU4's settings, then Steam's or Windows' CJK (#158, lab#93);
    // ManagerApp follows the chosen game folder later.
    anvil::i18n::set_language(anvil::i18n::choose_manager_language(anvil::eu4_user_dir(game)).language);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string package_error;
    std::optional<Package> package = load_embedded_package(package_error);

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // No white flash before the first frame and while resizing.
    window_class.hbrBackground = CreateSolidBrush(RGB(0x12, 0x11, 0x10));
    // The icon of the title bar, the taskbar and Alt+Tab.
    window_class.hIcon = static_cast<HICON>(LoadImageW(instance, L"ANVIL_ICON", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(instance, L"ANVIL_ICON", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON), 0));
    window_class.lpszClassName = L"AnvilManager";
    RegisterClassExW(&window_class);

    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi_x = 96, dpi_y = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (const HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (const auto get_dpi = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
            get_dpi(monitor, 0, &dpi_x, &dpi_y);
        }
    }
    g_scale = shot ? shot->scale / 100.0f : dpi_x / 96.0f;
    // The screenshot mode's window is a hidden popup: its client area is the whole window.
    const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Anvil Manager", shot ? WS_POPUP : WS_OVERLAPPEDWINDOW,
                                        shot ? 0 : CW_USEDEFAULT, shot ? 0 : CW_USEDEFAULT, static_cast<int>((shot ? shot->width : 1040) * g_scale),
                                        static_cast<int>(720 * g_scale), nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    if (!shot) g_scale = GetDpiForWindow(window) / 96.0f;

    Graphics graphics;
    if (!graphics.create(window)) {
        MessageBoxW(window, anvil::widen(anvil::i18n::tr("Direct3D 11 is not available on this computer.")).c_str(),
                    L"Anvil Manager", MB_ICONERROR);
        return 1;
    }
    g_graphics = &graphics;
    if (!shot) {
        style_caption(window);
        DragAcceptFiles(window, TRUE);
        ShowWindow(window, show);
        UpdateWindow(window);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the exe
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(graphics.device, graphics.context);

    std::optional<ManagerApp::Screenshot> screenshot;
    if (shot) screenshot = ManagerApp::Screenshot{shot->build};
    auto app = std::make_unique<ManagerApp>(window, std::move(package), package_error, game, screenshot);
    g_app = app.get();

    // The screenshot mode: the tabs to draw, each given frames to settle after
    // the folder is read; 60 s at most for the whole.
    std::vector<std::string> shot_tabs;
    if (shot) {
        std::error_code ec;
        fs::create_directories(shot->out, ec);
        if (!shot->tab.empty()) {
            shot_tabs.push_back(shot->tab);
        } else {
            shot_tabs.assign(std::begin(kShotTabs), std::end(kShotTabs));
        }
        app->show_tab(shot_tabs.front());
        if (!shot->select.empty()) app->select_plugin(shot->select);
        if (!shot->dialog.empty()) app->open_dialog(shot->dialog);
    }
    const auto shot_started = std::chrono::steady_clock::now();
    size_t shot_index = 0;
    int shot_frames = 0;
    int shot_failed = 0;

    bool done = false;
    while (!done) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (graphics.occluded && graphics.swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        graphics.occluded = false;
        if (graphics.resize_width != 0 && graphics.resize_height != 0) {
            graphics.release_target();
            graphics.swap_chain->ResizeBuffers(0, graphics.resize_width, graphics.resize_height, DXGI_FORMAT_UNKNOWN, 0);
            graphics.resize_width = graphics.resize_height = 0;
            graphics.create_target();
        }
        if (g_fonts_dirty || g_fonts_language != anvil::i18n::language()) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            load_fonts(g_scale);
            g_fonts_dirty = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app->draw();
        ImGui::Render();
        const float clear[4] = {anvil::ui::kBackground.x, anvil::ui::kBackground.y, anvil::ui::kBackground.z, 1.0f};
        graphics.context->OMSetRenderTargets(1, &graphics.target, nullptr);
        graphics.context->ClearRenderTargetView(graphics.target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (shot) {
            // Five frames after the folder is read: tab bars, child sizes and wrapping settle.
            // A dialog's dimming fades in over about 0.2 s of frame time: half a second of frames for it.
            shot_frames = app->settled() ? shot_frames + 1 : 0;
            if (!shot->dialog.empty()) Sleep(16);
            if (shot_frames >= (shot->dialog.empty() ? 5 : 30)) {
                const std::string dialog = shot->dialog.empty() ? std::string() : "-" + lower(shot->dialog);
                const fs::path file = shot->out / anvil::widen(lower(shot_tabs[shot_index]) + dialog + ".png");
                if (!write_png(graphics, file)) ++shot_failed;
                shot_frames = 0;
                if (++shot_index == shot_tabs.size()) break;
                app->show_tab(shot_tabs[shot_index]);
            }
            if (std::chrono::steady_clock::now() - shot_started > std::chrono::seconds(60)) {
                shot_failed = 100;
                break;
            }
            graphics.swap_chain->Present(0, 0);
            continue;
        }
        graphics.occluded = graphics.swap_chain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
        if (app->should_close()) DestroyWindow(window);
    }

    g_app = nullptr;
    app.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_graphics = nullptr;
    CoUninitialize();
    return shot_failed == 0 ? 0 : 1;
}
