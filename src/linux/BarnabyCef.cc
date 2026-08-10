#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_display_handler.h"
#include "include/cef_frame.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <cairo/cairo-xlib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr char kWindowTitle[] = "Barnaby";
constexpr char kServerUrlPrefix[] = "GITBOARD_SERVER_URL=";
constexpr int kSplashWidth = 360;
constexpr int kSplashHeight = 280;
constexpr int kSplashIconSize = 124;

struct helpers {
  fs::path gitboard;
  fs::path server;
};

struct app_state {
  pid_t server_pid = -1;
  std::string server_url;
  Display* splash_display = nullptr;
  Window splash_window = 0;
  GdkPixbuf* splash_icon = nullptr;
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefBrowserView> browser_view;
  CefRefPtr<CefWindow> window;
  CefRefPtr<CefClient> client;
  CefRefPtr<CefBrowserViewDelegate> browser_view_delegate;
  CefRefPtr<CefWindowDelegate> window_delegate;
};

fs::path executable_path() {
  std::array<char, 4096> buffer{};
  const ssize_t count = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (count < 0 || static_cast<std::size_t>(count) == buffer.size()) {
    throw std::runtime_error("could not determine the Barnaby executable path");
  }
  return fs::path(std::string(buffer.data(), static_cast<std::size_t>(count)));
}

bool executable_file(const fs::path& path) {
  return fs::exists(path) && access(path.c_str(), X_OK) == 0;
}

bool regular_file(const fs::path& path) {
  return fs::exists(path) && fs::is_regular_file(path);
}

fs::path runfiles_root(const fs::path& exe) {
  fs::path sibling = exe;
  sibling += ".runfiles";
  if (fs::is_directory(sibling)) return sibling;

  const char* runfiles_dir = std::getenv("RUNFILES_DIR");
  if (runfiles_dir && *runfiles_dir && fs::is_directory(runfiles_dir)) {
    return fs::path(runfiles_dir);
  }
  return {};
}

fs::path find_icon_path() {
  const fs::path exe = executable_path();
  const fs::path bin_dir = exe.parent_path();
  const fs::path runfiles = runfiles_root(exe);

  std::vector<fs::path> candidates = {
      bin_dir / "app_icon_128x128.png",
      bin_dir / "app_icon_256x256.png",
      bin_dir.parent_path() / "share" / "icons" / "hicolor" / "128x128" /
          "apps" / "dev.gitboard.barnaby.png",
      bin_dir.parent_path() / "share" / "icons" / "hicolor" / "256x256" /
          "apps" / "dev.gitboard.barnaby.png",
  };
  if (!runfiles.empty()) {
    candidates.push_back(runfiles / "_main" / "packaging" / "icons" /
                         "app_icon_128x128.png");
    candidates.push_back(runfiles / "_main" / "packaging" / "icons" /
                         "app_icon_256x256.png");
    candidates.push_back(runfiles / "gitboard_cpp" / "packaging" / "icons" /
                         "app_icon_128x128.png");
    candidates.push_back(runfiles / "gitboard_cpp" / "packaging" / "icons" /
                         "app_icon_256x256.png");
  }

  for (const fs::path& candidate : candidates) {
    if (regular_file(candidate)) return candidate;
  }
  return {};
}

helpers find_helpers() {
  const fs::path exe = executable_path();
  const fs::path bin_dir = exe.parent_path();
  const fs::path runfiles = runfiles_root(exe);

  std::vector<helpers> candidates = {
      {bin_dir / "gitboard", bin_dir / "gitboard-server"},
      {bin_dir.parent_path() / "gitboard",
       bin_dir.parent_path() / "gitboard-server"},
      {bin_dir.parent_path() / "lib" / "barnaby" / "gitboard",
       bin_dir.parent_path() / "lib" / "barnaby" / "gitboard-server"},
  };
  if (!runfiles.empty()) {
    candidates.push_back({runfiles / "_main" / "src" / "gitboard",
                          runfiles / "_main" / "src" / "gitboard-server"});
    candidates.push_back({runfiles / "gitboard_cpp" / "src" / "gitboard",
                          runfiles / "gitboard_cpp" / "src" /
                              "gitboard-server"});
  }

  for (const auto& candidate : candidates) {
    if (executable_file(candidate.gitboard) &&
        executable_file(candidate.server)) {
      return candidate;
    }
  }

  throw std::runtime_error(
      "could not find executable gitboard and gitboard-server helpers beside "
      "Barnaby or in a recognized Bazel or packaged layout");
}

void pump_splash_events(app_state* state);

std::string read_line(int fd, pid_t child, app_state* state) {
  std::string line;
  char c = '\0';
  while (true) {
    pump_splash_events(state);

    pollfd pfd{fd, POLLIN, 0};
    const int ready = poll(&pfd, 1, 10);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return line;
    }
    if (ready == 0) {
      int status = 0;
      if (waitpid(child, &status, WNOHANG) == child) return line;
      continue;
    }

    const ssize_t count = read(fd, &c, 1);
    if (count == 1) {
      if (c == '\n') return line;
      if (c != '\r') line.push_back(c);
      continue;
    }
    if (count == 0) return line;
    if (errno == EINTR) continue;

    int status = 0;
    if (waitpid(child, &status, WNOHANG) == child) return line;
    return line;
  }
}

void stop_server(app_state* state) {
  if (!state || state->server_pid <= 0) return;

  kill(state->server_pid, SIGTERM);
  for (int attempt = 0; attempt < 50; ++attempt) {
    int status = 0;
    const pid_t result = waitpid(state->server_pid, &status, WNOHANG);
    if (result == state->server_pid || (result < 0 && errno == ECHILD)) {
      state->server_pid = -1;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  kill(state->server_pid, SIGKILL);
  waitpid(state->server_pid, nullptr, 0);
  state->server_pid = -1;
}

void draw_splash(app_state* state) {
  if (!state || !state->splash_display || !state->splash_window) return;

  Display* display = state->splash_display;
  const int screen = DefaultScreen(display);
  cairo_surface_t* surface = cairo_xlib_surface_create(
      display, state->splash_window, DefaultVisual(display, screen),
      kSplashWidth, kSplashHeight);
  cairo_t* cr = cairo_create(surface);

  cairo_set_source_rgb(cr, 0.965, 0.965, 0.965);
  cairo_paint(cr);

  if (state->splash_icon) {
    const int width = gdk_pixbuf_get_width(state->splash_icon);
    const int height = gdk_pixbuf_get_height(state->splash_icon);
    const double x = (kSplashWidth - width) / 2.0;
    const double y = 48.0;

    const int channels = gdk_pixbuf_get_n_channels(state->splash_icon);
    const int rowstride = gdk_pixbuf_get_rowstride(state->splash_icon);
    const bool has_alpha = gdk_pixbuf_get_has_alpha(state->splash_icon);
    const guchar* pixels = gdk_pixbuf_get_pixels(state->splash_icon);
    const int cairo_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
                                                           width);
    std::vector<std::uint8_t> argb(
        static_cast<std::size_t>(cairo_stride) * height);

    for (int py = 0; py < height; ++py) {
      const guchar* src = pixels + py * rowstride;
      std::uint8_t* dst = argb.data() + py * cairo_stride;
      for (int px = 0; px < width; ++px) {
        const std::uint8_t r = src[px * channels + 0];
        const std::uint8_t g = src[px * channels + 1];
        const std::uint8_t b = src[px * channels + 2];
        const std::uint8_t a = has_alpha ? src[px * channels + 3] : 255;
        dst[px * 4 + 0] = static_cast<std::uint8_t>((b * a) / 255);
        dst[px * 4 + 1] = static_cast<std::uint8_t>((g * a) / 255);
        dst[px * 4 + 2] = static_cast<std::uint8_t>((r * a) / 255);
        dst[px * 4 + 3] = a;
      }
    }

    cairo_surface_t* image = cairo_image_surface_create_for_data(
        argb.data(), CAIRO_FORMAT_ARGB32, width, height, cairo_stride);
    cairo_set_source_surface(cr, image, x, y);
    cairo_paint(cr);
    cairo_surface_destroy(image);
  }

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 26.0);
  cairo_text_extents_t extents{};
  cairo_text_extents(cr, kWindowTitle, &extents);
  cairo_set_source_rgb(cr, 0.075, 0.075, 0.075);
  cairo_move_to(cr, (kSplashWidth - extents.width) / 2.0 - extents.x_bearing,
                216.0);
  cairo_show_text(cr, kWindowTitle);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  XFlush(display);
}

void pump_splash_events(app_state* state) {
  if (!state || !state->splash_display) return;

  while (XPending(state->splash_display) > 0) {
    XEvent event{};
    XNextEvent(state->splash_display, &event);
    if (event.type == Expose) {
      draw_splash(state);
    }
  }
}

void show_splash_screen(app_state* state) {
  if (!state) return;

  Display* display = XOpenDisplay(nullptr);
  if (!display) return;
  const int display_fd = ConnectionNumber(display);
  if (display_fd >= 0) {
    fcntl(display_fd, F_SETFD, fcntl(display_fd, F_GETFD) | FD_CLOEXEC);
  }

  const int screen = DefaultScreen(display);
  const int root_width = DisplayWidth(display, screen);
  const int root_height = DisplayHeight(display, screen);
  const int x = (root_width - kSplashWidth) / 2;
  const int y = (root_height - kSplashHeight) / 2;

  XSetWindowAttributes attributes{};
  attributes.override_redirect = True;
  attributes.background_pixel = WhitePixel(display, screen);
  attributes.event_mask = ExposureMask | StructureNotifyMask;
  Window window = XCreateWindow(
      display, RootWindow(display, screen), x, y, kSplashWidth, kSplashHeight,
      0, CopyFromParent, InputOutput, CopyFromParent,
      CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
  if (!window) {
    XCloseDisplay(display);
    return;
  }

  XStoreName(display, window, kWindowTitle);
  const Atom splash_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE_SPLASH", False);
  const Atom window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
  XChangeProperty(display, window, window_type, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&splash_type), 1);

  const fs::path icon_path = find_icon_path();
  if (!icon_path.empty()) {
    GError* error = nullptr;
    state->splash_icon = gdk_pixbuf_new_from_file_at_scale(
        icon_path.c_str(), kSplashIconSize, kSplashIconSize, TRUE, &error);
    if (error) g_error_free(error);
  }

  state->splash_display = display;
  state->splash_window = window;
  XMapRaised(display, window);
  draw_splash(state);
  pump_splash_events(state);
}

void dismiss_splash_screen(app_state* state) {
  if (!state) return;

  if (state->splash_display && state->splash_window) {
    XDestroyWindow(state->splash_display, state->splash_window);
    XFlush(state->splash_display);
    state->splash_window = 0;
  }
  if (state->splash_display) {
    XCloseDisplay(state->splash_display);
    state->splash_display = nullptr;
  }
  if (state->splash_icon) {
    g_object_unref(state->splash_icon);
    state->splash_icon = nullptr;
  }
}

void start_server(app_state* state) {
  const helpers paths = find_helpers();
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    throw std::runtime_error("could not create server startup pipe");
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    throw std::runtime_error("could not start the Barnaby server");
  }

  if (pid == 0) {
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    execl(paths.server.c_str(), paths.server.c_str(), "--port", "0",
          "--gitboard-path", paths.gitboard.c_str(), "--no-open-browser",
          static_cast<char*>(nullptr));
    _exit(127);
  }

  close(pipe_fds[1]);
  state->server_pid = pid;
  while (state->server_url.empty()) {
    const std::string line = read_line(pipe_fds[0], pid, state);
    if (line.empty()) break;
    if (line.rfind(kServerUrlPrefix, 0) == 0) {
      state->server_url = line.substr(sizeof(kServerUrlPrefix) - 1);
    }
  }
  close(pipe_fds[0]);

  if (state->server_url.empty()) {
    stop_server(state);
    throw std::runtime_error("the Barnaby server did not report its listening URL");
  }
}

fs::path find_cef_resources_dir() {
  const fs::path exe = executable_path();
  const fs::path bin_dir = exe.parent_path();
  const fs::path runfiles = runfiles_root(exe);

  std::vector<fs::path> candidates = {
      bin_dir,
      bin_dir / "Resources",
      bin_dir.parent_path() / "lib" / "barnaby" / "cef",
  };
  if (!runfiles.empty()) {
    candidates.push_back(runfiles / "+cef_config+local_cef" / "Resources");
    candidates.push_back(runfiles / "local_cef" / "Resources");
  }

  for (const fs::path& candidate : candidates) {
    if (regular_file(candidate / "resources.pak") &&
        fs::is_directory(candidate / "locales")) {
      return candidate;
    }
  }

  if (!runfiles.empty()) {
    for (const auto& entry : fs::recursive_directory_iterator(runfiles)) {
      if (entry.path().filename() == "resources.pak" &&
          fs::is_directory(entry.path().parent_path() / "locales")) {
        return entry.path().parent_path();
      }
    }
  }

  throw std::runtime_error("could not find Chromium resource files");
}

fs::path cache_dir() {
  const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
  if (xdg_cache && *xdg_cache) return fs::path(xdg_cache) / "Barnaby" / "CEF";

  const char* home = std::getenv("HOME");
  if (home && *home) return fs::path(home) / ".cache" / "Barnaby" / "CEF";

  return fs::temp_directory_path() / "Barnaby" / "CEF";
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
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    if (state_->browser && state_->browser->IsSame(browser)) {
      state_->browser = nullptr;
    }
    stop_server(state_);
    CefQuitMessageLoop();
  }

  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString&) override {}

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

class BarnabyBrowserViewDelegate : public CefBrowserViewDelegate {
 public:
  BarnabyBrowserViewDelegate() = default;

  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }

 private:
  IMPLEMENT_REFCOUNTING(BarnabyBrowserViewDelegate);
  DISALLOW_COPY_AND_ASSIGN(BarnabyBrowserViewDelegate);
};

class BarnabyWindowDelegate : public CefWindowDelegate {
 public:
  explicit BarnabyWindowDelegate(app_state* state) : state_(state) {}

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    state_->window = window;
    window->SetTitle(kWindowTitle);
    window->SetToFillLayout();
    window->AddChildView(state_->browser_view);
    window->CenterWindow(CefSize(1180, 760));
    window->Show();
    dismiss_splash_screen(state_);
    state_->browser_view->RequestFocus();
  }

  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    if (state_->window && state_->window->IsSame(window)) {
      state_->window = nullptr;
    }
    state_->browser_view = nullptr;
    stop_server(state_);
    CefQuitMessageLoop();
  }

  cef_runtime_style_t GetWindowRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }

 private:
  app_state* state_;

  IMPLEMENT_REFCOUNTING(BarnabyWindowDelegate);
  DISALLOW_COPY_AND_ASSIGN(BarnabyWindowDelegate);
};

void create_browser(app_state* state) {
  CefBrowserSettings browser_settings;
  state->client = new BarnabyCefClient(state);
  state->browser_view_delegate = new BarnabyBrowserViewDelegate();
  state->browser_view = CefBrowserView::CreateBrowserView(
      state->client, state->server_url, browser_settings, nullptr, nullptr,
      state->browser_view_delegate);
  if (!state->browser_view) {
    throw std::runtime_error("could not create Chromium browser view");
  }

  state->window_delegate = new BarnabyWindowDelegate(state);
  state->window = CefWindow::CreateTopLevelWindow(state->window_delegate);
  if (!state->window) {
    throw std::runtime_error("could not create Barnaby window");
  }
}

void show_error(const std::string& detail) {
  std::cerr << "Barnaby could not start: " << detail << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  CefMainArgs main_args(argc, argv);
  const int subprocess_exit = CefExecuteProcess(main_args, nullptr, nullptr);
  if (subprocess_exit >= 0) return subprocess_exit;

  app_state state;
  bool cef_initialized = false;

  try {
    show_splash_screen(&state);
    start_server(&state);

    const fs::path resources_dir = find_cef_resources_dir();
    const fs::path cef_cache_dir = cache_dir();
    fs::create_directories(cef_cache_dir);

    CefSettings settings;
    settings.no_sandbox = true;
    CefString(&settings.cache_path) = cef_cache_dir.string();
    CefString(&settings.resources_dir_path) = resources_dir.string();
    CefString(&settings.locales_dir_path) = (resources_dir / "locales").string();

    if (!CefInitialize(main_args, settings, nullptr, nullptr)) {
      throw std::runtime_error("could not initialize Chromium Embedded Framework");
    }
    cef_initialized = true;

    create_browser(&state);
    CefRunMessageLoop();
    CefShutdown();
    return 0;
  } catch (const std::exception& ex) {
    dismiss_splash_screen(&state);
    show_error(ex.what());
    stop_server(&state);
    if (cef_initialized) CefShutdown();
    return 1;
  }
}
