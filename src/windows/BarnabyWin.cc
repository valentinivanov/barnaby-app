#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_frame.h"
#include "include/cef_keyboard_handler.h"
#include "src/windows/resource.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr wchar_t kWindowClass[] = L"BarnabyWindow";
constexpr wchar_t kSplashWindowClass[] = L"BarnabySplashWindow";
constexpr wchar_t kWindowTitle[] = L"Barnaby";
constexpr char kServerUrlPrefix[] = "GITBOARD_SERVER_URL=";
constexpr int kSplashWidth = 360;
constexpr int kSplashHeight = 280;
constexpr int kSplashIconSize = 124;

struct helpers {
  fs::path gitboard;
  fs::path server;
};

struct app_state {
  HWND window = nullptr;
  HWND splash_window = nullptr;
  HICON app_icon = nullptr;
  PROCESS_INFORMATION server{};
  bool server_started = false;
  std::string server_url;
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefClient> client;
};

void pump_pending_messages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

bool is_cef_subprocess() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;

  bool is_subprocess = false;
  for (int i = 1; i < argc; ++i) {
    if (wcsncmp(argv[i], L"--type=", 7) == 0) {
      is_subprocess = true;
      break;
    }
  }
  LocalFree(argv);
  return is_subprocess;
}

std::wstring widen(const std::string& value) {
  if (value.empty()) return L"";
  int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                 static_cast<int>(value.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), size);
  return out;
}

std::wstring quote_arg(const std::wstring& arg) {
  if (arg.empty()) return L"\"\"";
  if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;

  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
    } else if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(c);
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      backslashes = 0;
      out.push_back(c);
    }
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring command_line_for(const fs::path& executable,
                              const std::vector<std::string>& args) {
  std::wstring line = quote_arg(executable.wstring());
  for (const auto& arg : args) {
    line.push_back(L' ');
    line += quote_arg(widen(arg));
  }
  return line;
}

fs::path executable_directory() {
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                    static_cast<DWORD>(buffer.size()));
  while (length == buffer.size()) {
    buffer.resize(buffer.size() * 2);
    length = GetModuleFileNameW(nullptr, buffer.data(),
                                static_cast<DWORD>(buffer.size()));
  }
  if (length == 0) {
    throw std::runtime_error("could not determine the Barnaby executable path");
  }
  buffer.resize(length);
  return fs::path(buffer).parent_path();
}

bool usable_helper(const fs::path& path) {
  return fs::exists(path) && fs::is_regular_file(path);
}

helpers find_helpers() {
  const fs::path bin_dir = executable_directory();
  const std::vector<helpers> candidates = {
      {bin_dir / "gitboard.exe", bin_dir / "gitboard-server.exe"},
      {bin_dir.parent_path() / "gitboard.exe",
       bin_dir.parent_path() / "gitboard-server.exe"},
      {bin_dir / "gitboard", bin_dir / "gitboard-server"},
      {bin_dir.parent_path() / "gitboard",
       bin_dir.parent_path() / "gitboard-server"},
  };
  for (const auto& candidate : candidates) {
    if (usable_helper(candidate.gitboard) && usable_helper(candidate.server)) {
      return candidate;
    }
  }
  throw std::runtime_error(
      "could not find executable gitboard and gitboard-server helpers beside "
      "Barnaby or in a recognized Bazel layout");
}

bool read_line(HANDLE pipe, HANDLE process, std::string* line) {
  line->clear();
  char c = '\0';
  DWORD read = 0;
  while (true) {
    // The splash window is shown before server startup, so keep its UI alive
    // while waiting for the helper to print the startup URL.
    pump_pending_messages();

    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
      return !line->empty();
    }
    if (available == 0) {
      DWORD exit_code = 0;
      if (process && GetExitCodeProcess(process, &exit_code) &&
          exit_code != STILL_ACTIVE) {
        return !line->empty();
      }
      Sleep(10);
      continue;
    }

    if (!ReadFile(pipe, &c, 1, &read, nullptr) || read != 1) {
      return !line->empty();
    }
    if (c == '\n') return true;
    if (c != '\r') line->push_back(c);
  }
}

void stop_server(app_state* state) {
  if (!state->server_started) return;
  TerminateProcess(state->server.hProcess, 0);
  WaitForSingleObject(state->server.hProcess, INFINITE);
  CloseHandle(state->server.hThread);
  CloseHandle(state->server.hProcess);
  state->server_started = false;
}

void start_server(app_state* state) {
  const helpers paths = find_helpers();

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
    throw std::runtime_error("could not create server startup pipe");
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  std::wstring command = command_line_for(
      paths.server, {"--port", "0", "--gitboard-path", paths.gitboard.string(),
                     "--no-open-browser"});

  BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                &state->server);
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    throw std::runtime_error("could not start the Barnaby server");
  }
  state->server_started = true;

  while (state->server_url.empty()) {
    std::string line;
    if (!read_line(read_pipe, state->server.hProcess, &line)) break;
    if (line.rfind(kServerUrlPrefix, 0) == 0) {
      state->server_url = line.substr(sizeof(kServerUrlPrefix) - 1);
    }
  }
  CloseHandle(read_pipe);

  if (state->server_url.empty()) {
    stop_server(state);
    throw std::runtime_error("the Barnaby server did not report its listening URL");
  }
}

void show_error(const std::string& detail) {
  MessageBoxW(nullptr, widen(detail).c_str(), L"Barnaby could not start",
              MB_ICONERROR | MB_OK);
}

fs::path local_app_data_dir() {
  const wchar_t* local_app_data = _wgetenv(L"LOCALAPPDATA");
  if (local_app_data && *local_app_data) {
    return fs::path(local_app_data) / "Barnaby";
  }
  return fs::temp_directory_path() / "Barnaby";
}

void resize_browser(app_state* state) {
  if (!state || !state->browser || !state->window) return;
  CefWindowHandle browser_window = state->browser->GetHost()->GetWindowHandle();
  if (!browser_window) return;
  RECT bounds{};
  GetClientRect(state->window, &bounds);
  SetWindowPos(browser_window, nullptr, 0, 0, bounds.right - bounds.left,
               bounds.bottom - bounds.top, SWP_NOZORDER);
}

void paint_splash(HWND hwnd) {
  app_state* state = reinterpret_cast<app_state*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(hwnd, &ps);
  RECT bounds{};
  GetClientRect(hwnd, &bounds);

  HBRUSH background = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  FillRect(dc, &bounds, background);

  HICON icon = state ? state->app_icon : nullptr;
  if (icon) {
    const int icon_x = (bounds.right - kSplashIconSize) / 2;
    const int icon_y = 48;
    DrawIconEx(dc, icon_x, icon_y, icon, kSplashIconSize, kSplashIconSize, 0,
               nullptr, DI_NORMAL);
  }

  HFONT font = CreateFontW(
      26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HFONT old_font = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
  RECT text_bounds{40, 196, bounds.right - 40, 236};
  DrawTextW(dc, kWindowTitle, -1, &text_bounds,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  if (old_font) SelectObject(dc, old_font);
  if (font) DeleteObject(font);

  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK splash_window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  if (message == WM_CREATE) {
    CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    app_state* state = static_cast<app_state*>(create->lpCreateParams);
    state->splash_window = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(state));
    return 0;
  }
  if (message == WM_PAINT) {
    paint_splash(hwnd);
    return 0;
  }
  if (message == WM_ERASEBKGND) {
    return 1;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void register_splash_window_class(HINSTANCE instance) {
  WNDCLASSW wc{};
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = splash_window_proc;
  wc.hInstance = instance;
  wc.lpszClassName = kSplashWindowClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    throw std::runtime_error("could not register Barnaby splash window class");
  }
}

void show_splash_screen(HINSTANCE instance, app_state* state) {
  register_splash_window_class(instance);

  state->app_icon = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(IDI_BARNABY_APP), IMAGE_ICON, kSplashIconSize,
      kSplashIconSize, LR_DEFAULTCOLOR));

  const int screen_width = GetSystemMetrics(SM_CXSCREEN);
  const int screen_height = GetSystemMetrics(SM_CYSCREEN);
  const int x = (screen_width - kSplashWidth) / 2;
  const int y = (screen_height - kSplashHeight) / 2;

  HWND splash = CreateWindowExW(
      WS_EX_TOOLWINDOW, kSplashWindowClass, kWindowTitle, WS_POPUP, x, y,
      kSplashWidth, kSplashHeight, nullptr, nullptr, instance, state);
  if (!splash) {
    throw std::runtime_error("could not create Barnaby splash window");
  }

  ShowWindow(splash, SW_SHOWNORMAL);
  UpdateWindow(splash);
  pump_pending_messages();
}

void dismiss_splash_screen(app_state* state) {
  if (state && state->splash_window) {
    DestroyWindow(state->splash_window);
    state->splash_window = nullptr;
  }
  if (state && state->app_icon) {
    DestroyIcon(state->app_icon);
    state->app_icon = nullptr;
  }
}

class BarnabyCefClient : public CefClient,
                         public CefLifeSpanHandler,
                         public CefDisplayHandler,
                         public CefKeyboardHandler {
 public:
  explicit BarnabyCefClient(app_state* state) : state_(state) {}

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    state_->browser = browser;
    resize_browser(state_);
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    if (state_->browser && state_->browser->IsSame(browser)) {
      state_->browser = nullptr;
    }
  }

  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
    if (state_->window) SetWindowTextW(state_->window, title.ToWString().c_str());
  }

  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                     const CefKeyEvent& event,
                     CefEventHandle,
                     bool*) override {
    if (!browser || event.type != KEYEVENT_RAWKEYDOWN) return false;
    if ((event.modifiers & EVENTFLAG_ALT_DOWN) != 0) return false;
    if ((event.modifiers & EVENTFLAG_CONTROL_DOWN) == 0) return false;

    CefRefPtr<CefFrame> frame = browser->GetFocusedFrame();
    if (!frame) frame = browser->GetMainFrame();
    if (!frame) return false;

    const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
    switch (event.windows_key_code) {
      case 'A':
        frame->SelectAll();
        return true;
      case 'C':
        frame->Copy();
        return true;
      case 'V':
        frame->Paste();
        return true;
      case 'X':
        frame->Cut();
        return true;
      case 'Y':
        frame->Redo();
        return true;
      case 'Z':
        if (shift) {
          frame->Redo();
        } else {
          frame->Undo();
        }
        return true;
      default:
        return false;
    }
  }

 private:
  app_state* state_;

  IMPLEMENT_REFCOUNTING(BarnabyCefClient);
  DISALLOW_COPY_AND_ASSIGN(BarnabyCefClient);
};

void initialize_browser(app_state* state) {
  RECT bounds{};
  GetClientRect(state->window, &bounds);

  CefWindowInfo window_info;
  window_info.SetAsChild(state->window,
                         CefRect(0, 0, bounds.right - bounds.left,
                                 bounds.bottom - bounds.top));

  CefBrowserSettings browser_settings;
  state->client = new BarnabyCefClient(state);
  bool created = CefBrowserHost::CreateBrowser(
      window_info, state->client, state->server_url, browser_settings, nullptr,
      nullptr);
  if (!created) {
    throw std::runtime_error("could not create Chromium browser");
  }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  if (message == WM_CREATE) {
    CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    app_state* state = static_cast<app_state*>(create->lpCreateParams);
    state->window = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(state));
    return 0;
  }
  if (message == WM_SIZE) {
    app_state* state = reinterpret_cast<app_state*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    resize_browser(state);
    return 0;
  }
  if (message == WM_DESTROY) {
    app_state* state = reinterpret_cast<app_state*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
      state->browser = nullptr;
      state->client = nullptr;
    }
    stop_server(state);
    CefQuitMessageLoop();
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

int run(HINSTANCE instance, int show_command, app_state* state) {
  try {
    start_server(state);
  } catch (...) {
    dismiss_splash_screen(state);
    throw;
  }

  WNDCLASSW wc{};
  wc.lpfnWndProc = window_proc;
  wc.hInstance = instance;
  wc.lpszClassName = kWindowClass;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_BARNABY_APP));
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  if (!RegisterClassW(&wc)) {
    dismiss_splash_screen(state);
    stop_server(state);
    throw std::runtime_error("could not register Barnaby window class");
  }

  HWND window = CreateWindowExW(
      0, kWindowClass, kWindowTitle,
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr,
      nullptr, instance, state);
  if (!window) {
    dismiss_splash_screen(state);
    stop_server(state);
    throw std::runtime_error("could not create Barnaby window");
  }

  ShowWindow(window, show_command);
  UpdateWindow(window);
  dismiss_splash_screen(state);

  try {
    initialize_browser(state);
  } catch (...) {
    stop_server(state);
    throw;
  }

  CefRunMessageLoop();
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  const bool cef_subprocess = is_cef_subprocess();
  app_state state;
  if (!cef_subprocess) {
    try {
      show_splash_screen(instance, &state);
    } catch (const std::exception& ex) {
      show_error(ex.what());
      return 1;
    }
  }

  CefMainArgs main_args(instance);
  int exit_code = CefExecuteProcess(main_args, nullptr, nullptr);
  if (exit_code >= 0) {
    dismiss_splash_screen(&state);
    return exit_code;
  }
  pump_pending_messages();

  fs::path exe_dir = executable_directory();
  fs::path cache_dir = local_app_data_dir() / "CEF";
  fs::create_directories(cache_dir);

  CefSettings settings;
  settings.no_sandbox = true;
  CefString(&settings.cache_path) = cache_dir.wstring();
  CefString(&settings.resources_dir_path) = exe_dir.wstring();
  CefString(&settings.locales_dir_path) = (exe_dir / "locales").wstring();

  if (!CefInitialize(main_args, settings, nullptr, nullptr)) {
    dismiss_splash_screen(&state);
    show_error("could not initialize Chromium Embedded Framework");
    return 1;
  }
  pump_pending_messages();

  try {
    int result = run(instance, show_command, &state);
    CefShutdown();
    return result;
  } catch (const std::exception& ex) {
    dismiss_splash_screen(&state);
    show_error(ex.what());
    CefShutdown();
    return 1;
  }
}
