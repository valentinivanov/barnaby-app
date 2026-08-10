#include <gio/gio.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

constexpr char kApplicationId[] = "dev.gitboard.barnaby";
constexpr char kServerUrlPrefix[] = "GITBOARD_SERVER_URL=";
constexpr int kInitialLoadRetries = 10;

struct helpers {
  fs::path gitboard;
  fs::path server;
};

struct app_state {
  GtkApplication* application = nullptr;
  GtkWindow* window = nullptr;
  WebKitWebView* web_view = nullptr;
  GSubprocess* server = nullptr;
  GCancellable* server_wait = nullptr;
  std::string server_url;
  int load_retries = 0;
  bool page_loaded = false;
  bool fatal_error_shown = false;
  bool shutting_down = false;
};

fs::path executable_directory() {
  std::array<char, 4096> buffer{};
  const ssize_t count = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (count < 0 || static_cast<std::size_t>(count) == buffer.size()) {
    throw std::runtime_error("could not determine the Barnaby executable path");
  }
  return fs::path(std::string(buffer.data(), static_cast<std::size_t>(count))).parent_path();
}

bool usable_helper(const fs::path& path) {
  return fs::exists(path) && access(path.c_str(), X_OK) == 0;
}

helpers find_helpers() {
  const fs::path bin_dir = executable_directory();
  helpers development{bin_dir / "gitboard", bin_dir / "gitboard-server"};
  if (usable_helper(development.gitboard) && usable_helper(development.server)) {
    return development;
  }

  helpers bazel_output{bin_dir.parent_path() / "gitboard",
                       bin_dir.parent_path() / "gitboard-server"};
  if (usable_helper(bazel_output.gitboard) && usable_helper(bazel_output.server)) {
    return bazel_output;
  }

  const fs::path packaged_dir = bin_dir.parent_path() / "lib" / "barnaby";
  helpers packaged{packaged_dir / "gitboard", packaged_dir / "gitboard-server"};
  if (usable_helper(packaged.gitboard) && usable_helper(packaged.server)) {
    return packaged;
  }

  throw std::runtime_error(
      "could not find executable gitboard and gitboard-server helpers beside Barnaby "
      "or in a recognized Bazel or packaged layout");
}

void quit_after_alert(GObject* source, GAsyncResult* result, gpointer data) {
  GError* error = nullptr;
  gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
  if (error) g_error_free(error);
  g_application_quit(G_APPLICATION(static_cast<app_state*>(data)->application));
}

void show_fatal_error(app_state* state, const std::string& detail) {
  if (state->fatal_error_shown) return;
  state->fatal_error_shown = true;

  GtkAlertDialog* alert = gtk_alert_dialog_new("Barnaby could not start");
  gtk_alert_dialog_set_detail(alert, detail.c_str());
  const char* buttons[] = {"Quit", nullptr};
  gtk_alert_dialog_set_buttons(alert, buttons);
  gtk_alert_dialog_choose(alert, state->window, nullptr, quit_after_alert, state);
  g_object_unref(alert);
}

void stop_server(app_state* state) {
  if (!state->server) return;
  if (state->server_wait) g_cancellable_cancel(state->server_wait);
  g_subprocess_force_exit(state->server);
  g_subprocess_wait(state->server, nullptr, nullptr);
  g_clear_object(&state->server);
}

void server_finished(GObject* source, GAsyncResult* result, gpointer data) {
  app_state* state = static_cast<app_state*>(data);
  GError* error = nullptr;
  const gboolean succeeded =
      g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);
  if (!state->shutting_down) {
    std::string detail = "The Barnaby server stopped unexpectedly.";
    if (!succeeded && error && error->domain != G_IO_ERROR) {
      detail += " ";
      detail += error->message;
    }
    show_fatal_error(state, detail);
  }
  if (error) g_error_free(error);
}

void start_server(app_state* state) {
  const helpers paths = find_helpers();
  GError* error = nullptr;
  state->server = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, paths.server.c_str(), "--port", "0",
      "--gitboard-path", paths.gitboard.c_str(), "--no-open-browser", nullptr);
  if (!state->server) {
    std::string detail = "Could not start the Barnaby server.";
    if (error) {
      detail += " ";
      detail += error->message;
      g_error_free(error);
    }
    throw std::runtime_error(detail);
  }

  GDataInputStream* output =
      g_data_input_stream_new(g_subprocess_get_stdout_pipe(state->server));
  while (state->server_url.empty()) {
    gsize length = 0;
    gchar* line = g_data_input_stream_read_line(output, &length, nullptr, &error);
    if (!line) break;
    std::string text(line, length);
    g_free(line);
    if (text.rfind(kServerUrlPrefix, 0) == 0) {
      state->server_url = text.substr(sizeof(kServerUrlPrefix) - 1);
    }
  }
  g_object_unref(output);

  if (state->server_url.empty()) {
    std::string detail = "The Barnaby server did not report its listening URL.";
    if (error) {
      detail += " ";
      detail += error->message;
      g_error_free(error);
    }
    throw std::runtime_error(detail);
  }
  if (error) g_error_free(error);

  state->server_wait = g_cancellable_new();
  g_subprocess_wait_check_async(state->server, state->server_wait, server_finished, state);
}

gboolean retry_initial_load(gpointer data) {
  app_state* state = static_cast<app_state*>(data);
  if (!state->shutting_down && !state->fatal_error_shown && !state->page_loaded) {
    webkit_web_view_load_uri(state->web_view, state->server_url.c_str());
  }
  return G_SOURCE_REMOVE;
}

gboolean load_failed(WebKitWebView*, WebKitLoadEvent, const gchar*, GError* error,
                     gpointer data) {
  app_state* state = static_cast<app_state*>(data);
  if (!state->page_loaded && state->load_retries++ < kInitialLoadRetries) {
    g_timeout_add(150, retry_initial_load, state);
    return TRUE;
  }
  show_fatal_error(state, std::string("Could not load the Barnaby interface. ") +
                              (error ? error->message : ""));
  return TRUE;
}

void load_changed(WebKitWebView*, WebKitLoadEvent event, gpointer data) {
  if (event == WEBKIT_LOAD_FINISHED) {
    static_cast<app_state*>(data)->page_loaded = true;
  }
}

void activate(GtkApplication* application, gpointer data) {
  app_state* state = static_cast<app_state*>(data);
  state->window = GTK_WINDOW(gtk_application_window_new(application));
  gtk_window_set_title(state->window, "Barnaby");
  gtk_window_set_default_size(state->window, 1180, 760);

  try {
    start_server(state);
  } catch (const std::exception& ex) {
    gtk_window_present(state->window);
    show_fatal_error(state, ex.what());
    return;
  }

  state->web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
  g_signal_connect(state->web_view, "load-failed", G_CALLBACK(load_failed), state);
  g_signal_connect(state->web_view, "load-changed", G_CALLBACK(load_changed), state);
  gtk_window_set_child(state->window, GTK_WIDGET(state->web_view));
  gtk_window_present(state->window);
  webkit_web_view_load_uri(state->web_view, state->server_url.c_str());
}

void shutdown(GApplication*, gpointer data) {
  app_state* state = static_cast<app_state*>(data);
  state->shutting_down = true;
  stop_server(state);
}

}  // namespace

int main(int argc, char** argv) {
  app_state state;
  state.application = gtk_application_new(kApplicationId, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(state.application, "activate", G_CALLBACK(activate), &state);
  g_signal_connect(state.application, "shutdown", G_CALLBACK(shutdown), &state);
  const int result = g_application_run(G_APPLICATION(state.application), argc, argv);
  g_clear_object(&state.server_wait);
  g_clear_object(&state.application);
  return result;
}
