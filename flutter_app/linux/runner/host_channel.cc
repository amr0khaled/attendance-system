#include "host_channel.h"

#include <flutter_linux/flutter_linux.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/utsname.h>
#include <unistd.h>

// ---- helpers ---------------------------------------------------------------

static std::string exe_dir() {
  char buf[4096];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0)
    return ".";
  buf[len] = '\0';
  std::string p(buf);
  auto pos = p.rfind('/');
  return pos != std::string::npos ? p.substr(0, pos) : ".";
}

static std::string files_dir() {
  const gchar *data = g_get_user_data_dir();
  std::string d = std::string(data) + "/attendance_host";
  g_mkdir_with_parents(d.c_str(), 0755);
  return d;
}

static std::string basename_of(const std::string &path) {
  auto pos = path.rfind('/');
  return pos != std::string::npos ? path.substr(pos + 1) : path;
}

static void respond_success(FlMethodCall *method_call, FlValue *result) {
  g_autoptr(FlMethodResponse) response =
      FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond(method_call, response, &error))
    g_warning("host_channel: respond failed: %s", error->message);
}

static void respond_error(FlMethodCall *method_call, const char *msg) {
  g_autoptr(FlMethodResponse) response =
      FL_METHOD_RESPONSE(fl_method_error_response_new("host", msg, nullptr));
  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond(method_call, response, &error))
    g_warning("host_channel: respond failed: %s", error->message);
}

// ---- method handlers -------------------------------------------------------

static void handle_paths(FlMethodCall *method_call) {
  std::string bundle = exe_dir();
  std::string files = files_dir();
  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "nativeLibDir",
                           fl_value_new_string((bundle + "/lib").c_str()));
  fl_value_set_string_take(result, "filesDir",
                           fl_value_new_string(files.c_str()));
  respond_success(method_call, result);
}

static void handle_noop(FlMethodCall *method_call) {
  g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
  respond_success(method_call, result);
}

static void handle_alert(FlMethodCall *method_call) {
  FlValue *args = fl_method_call_get_args(method_call);
  FlValue *title_v = fl_value_lookup_string(args, "title");
  FlValue *text_v = fl_value_lookup_string(args, "text");
  const gchar *title = title_v ? fl_value_get_string(title_v) : "";
  const gchar *text = text_v ? fl_value_get_string(text_v) : "";

  const gchar *argv[] = {"notify-send", title, text, nullptr};
  g_spawn_async(nullptr, const_cast<gchar **>(argv), nullptr,
                G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr);

  g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
  respond_success(method_call, result);
}

static void handle_save_image(FlMethodCall *method_call) {
  FlValue *args = fl_method_call_get_args(method_call);
  FlValue *bytes_v = fl_value_lookup_string(args, "bytes");
  FlValue *name_v = fl_value_lookup_string(args, "name");
  if (!bytes_v || !name_v) {
    respond_error(method_call, "missing bytes or name");
    return;
  }

  std::string dir = std::string(g_get_home_dir()) + "/Pictures/Attendance";
  g_mkdir_with_parents(dir.c_str(), 0755);
  std::string path = dir + "/" + fl_value_get_string(name_v);

  const uint8_t *data = fl_value_get_uint8_list(bytes_v);
  size_t len = fl_value_get_length(bytes_v);

  std::ofstream f(path, std::ios::binary);
  if (!f) {
    respond_error(method_call, "could not write image file");
    return;
  }
  f.write(reinterpret_cast<const char *>(data),
          static_cast<std::streamsize>(len));
  f.close();

  g_autoptr(FlValue) result = fl_value_new_string(path.c_str());
  respond_success(method_call, result);
}

static void handle_save_to_downloads(FlMethodCall *method_call) {
  FlValue *args = fl_method_call_get_args(method_call);
  FlValue *path_v = fl_value_lookup_string(args, "path");
  if (!path_v) {
    respond_error(method_call, "missing path");
    return;
  }

  std::string src_path = fl_value_get_string(path_v);
  std::string name = basename_of(src_path);
  std::string dir = std::string(g_get_home_dir()) + "/Downloads/Attendance";
  g_mkdir_with_parents(dir.c_str(), 0755);
  std::string dest_path = dir + "/" + name;

  std::ifstream src(src_path, std::ios::binary);
  if (!src) {
    respond_error(method_call, "source file not found");
    return;
  }
  std::ofstream dst(dest_path, std::ios::binary);
  if (!dst) {
    respond_error(method_call, "could not write to Downloads");
    return;
  }
  dst << src.rdbuf();

  g_autoptr(FlValue) result = fl_value_new_string(dest_path.c_str());
  respond_success(method_call, result);
}

static void handle_pick_text_file(FlMethodCall *method_call) {
  gchar *stdout_str = nullptr;
  gint wait_status = 0;

  const gchar *argv[] = {"zenity", "--file-selection",
                         "--title=Open class list",
                         "--file-filter=CSV/Text | *.csv *.txt", nullptr};

  gboolean ok = g_spawn_sync(nullptr, const_cast<gchar **>(argv), nullptr,
                             G_SPAWN_SEARCH_PATH, nullptr, nullptr, &stdout_str,
                             nullptr, &wait_status, nullptr);

  if (!ok || !g_spawn_check_wait_status(wait_status, nullptr) || !stdout_str ||
      stdout_str[0] == '\0') {
    g_free(stdout_str);
    g_autoptr(FlValue) result = fl_value_new_null();
    respond_success(method_call, result);
    return;
  }

  g_strstrip(stdout_str);
  std::string path(stdout_str);
  g_free(stdout_str);

  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    respond_error(method_call, "could not read selected file");
    return;
  }

  auto size = f.tellg();
  if (size > 2 * 1024 * 1024) {
    respond_error(method_call, "file larger than 2 MB");
    return;
  }

  f.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.read(reinterpret_cast<char *>(data.data()), size);
  f.close();

  std::string name = basename_of(path);

  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "name", fl_value_new_string(name.c_str()));
  fl_value_set_string_take(result, "bytes",
                           fl_value_new_uint8_list(data.data(), data.size()));
  respond_success(method_call, result);
}

static void handle_open_url(FlMethodCall *method_call) {
  FlValue *args = fl_method_call_get_args(method_call);
  FlValue *url_v = fl_value_lookup_string(args, "url");
  if (url_v) {
    const gchar *url = fl_value_get_string(url_v);
    const gchar *argv[] = {"xdg-open", url, nullptr};
    g_spawn_async(nullptr, const_cast<gchar **>(argv), nullptr,
                  G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr);
  }
  g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
  respond_success(method_call, result);
}

static void handle_device_info(FlMethodCall *method_call) {
  std::string pretty_name = "Linux";
  std::ifstream os_rel("/etc/os-release");
  if (os_rel) {
    std::string line;
    while (std::getline(os_rel, line)) {
      if (line.rfind("PRETTY_NAME=", 0) == 0) {
        pretty_name = line.substr(12);
        if (pretty_name.size() >= 2 && pretty_name.front() == '"')
          pretty_name = pretty_name.substr(1, pretty_name.size() - 2);
        break;
      }
    }
  }

  char hostname[256] = {};
  gethostname(hostname, sizeof(hostname) - 1);

  struct utsname uts = {};
  uname(&uts);

  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "manufacturer",
                           fl_value_new_string("Linux"));
  fl_value_set_string_take(result, "model", fl_value_new_string(hostname));
  fl_value_set_string_take(result, "android",
                           fl_value_new_string(pretty_name.c_str()));
  fl_value_set_string_take(result, "sdk", fl_value_new_int(0));
  fl_value_set_string_take(result, "abis", fl_value_new_string(uts.machine));
  fl_value_set_string_take(result, "appVersion", fl_value_new_string("1.0.0"));
  fl_value_set_string_take(result, "batteryOptimizationIgnored",
                           fl_value_new_bool(TRUE));
  fl_value_set_string_take(result, "notificationsGranted",
                           fl_value_new_bool(TRUE));
  fl_value_set_string_take(result, "serviceRunning", fl_value_new_bool(FALSE));
  fl_value_set_string_take(result, "uiAttached", fl_value_new_bool(TRUE));
  respond_success(method_call, result);
}

// ---- channel dispatch ------------------------------------------------------

static void method_call_cb(FlMethodChannel *channel, FlMethodCall *method_call,
                           gpointer user_data) {
  const gchar *method = fl_method_call_get_name(method_call);

  if (strcmp(method, "paths") == 0) {
    handle_paths(method_call);
  } else if (strcmp(method, "startService") == 0 ||
             strcmp(method, "stopService") == 0 ||
             strcmp(method, "updateNotification") == 0 ||
             strcmp(method, "requestNotifications") == 0 ||
             strcmp(method, "requestBatteryExemption") == 0) {
    handle_noop(method_call);
  } else if (strcmp(method, "alert") == 0) {
    handle_alert(method_call);
  } else if (strcmp(method, "saveImage") == 0) {
    handle_save_image(method_call);
  } else if (strcmp(method, "saveToDownloads") == 0) {
    handle_save_to_downloads(method_call);
  } else if (strcmp(method, "pickTextFile") == 0) {
    handle_pick_text_file(method_call);
  } else if (strcmp(method, "openSettings") == 0) {
    // No settings screens to open on Linux desktop.
    g_autoptr(FlValue) r = fl_value_new_string("none");
    respond_success(method_call, r);
  } else if (strcmp(method, "openUrl") == 0) {
    handle_open_url(method_call);
  } else if (strcmp(method, "deviceInfo") == 0) {
    handle_device_info(method_call);
  } else {
    g_autoptr(FlMethodResponse) response =
        FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
    g_autoptr(GError) error = nullptr;
    fl_method_call_respond(method_call, response, &error);
  }
}

void host_channel_register(FlBinaryMessenger *messenger) {
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  // Channel must outlive the messenger — intentionally not using g_autoptr.
  FlMethodChannel *channel = fl_method_channel_new(messenger, "attendance/host",
                                                   FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb, nullptr,
                                            nullptr);
}
