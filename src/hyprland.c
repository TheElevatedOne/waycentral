#include "../waycentral.h"
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <json-c/json.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define POLL_MS 1000

/* Fills addr with the path of Hyprland's socket (.socket.sock or
 * .socket2.sock). Returns 0, or -1 when not running under Hyprland. */
static int socket_addr(struct sockaddr_un *addr, const char *name) {
  const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (!sig || !*sig)
    return -1;

  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;

  char path[sizeof(addr->sun_path)];
  if (runtime && *runtime) {
    snprintf(path, sizeof(path), "%s/hypr/%s/%s", runtime, sig, name);
    if (access(path, F_OK) != 0)
      snprintf(path, sizeof(path), "/tmp/hypr/%s/%s", sig, name);
  } else {
    snprintf(path, sizeof(path), "/tmp/hypr/%s/%s", sig, name);
  }
  memcpy(addr->sun_path, path, sizeof(path));
  return 0;
}

static int hypr_connect(const char *name) {
  struct sockaddr_un addr;
  if (socket_addr(&addr, name) != 0)
    return -1;

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* Sends a request like "j/activeworkspace" and parses the JSON reply */
static json_object *hypr_request(const char *request) {
  int fd = hypr_connect(".socket.sock");
  if (fd < 0)
    return NULL;

  size_t len = strlen(request);
  if (write(fd, request, len) != (ssize_t)len) {
    close(fd);
    return NULL;
  }

  struct wc_buf reply = {0};
  char chunk[4096];
  ssize_t n;
  while ((n = read(fd, chunk, sizeof(chunk))) > 0 ||
         (n < 0 && errno == EINTR))
    if (n > 0)
      wc_buf_appendf(&reply, "%.*s", (int)n, chunk);
  close(fd);

  char *text = wc_buf_finish(&reply);
  if (!text)
    return NULL;
  json_object *obj = json_tokener_parse(text);
  free(text);
  return obj;
}

static const char *json_str(json_object *obj, const char *key) {
  json_object *val;
  if (!obj || !json_object_object_get_ex(obj, key, &val))
    return NULL;
  return json_object_get_string(val);
}

int wc_hypr_layout_get(struct wc_hypr_layout *out) {
  memset(out, 0, sizeof(*out));

  json_object *ws = hypr_request("j/activeworkspace");
  if (!ws)
    return -1;

  json_object *id;
  if (json_object_object_get_ex(ws, "id", &id))
    out->workspace = json_object_get_int(id);
  const char *name = json_str(ws, "name");
  snprintf(out->workspace_name, sizeof(out->workspace_name), "%s",
           name ? name : "");

  const char *layout = json_str(ws, "tiledLayout");
  if (layout && *layout)
    snprintf(out->name, sizeof(out->name), "%s", layout);
  json_object_put(ws);

  if (!out->name[0]) {
    // Older Hyprland: only the global layout is known
    json_object *opt = hypr_request("j/getoption general:layout");
    const char *global = json_str(opt, "str");
    snprintf(out->name, sizeof(out->name), "%s", global ? global : "");
    json_object_put(opt);
  }

  return out->name[0] ? 0 : -1;
}

static const struct {
  const char *name;
  const char *letter;
  const char *title;
} layouts[] = {
    {"dwindle", "D", "Dwindle"},
    {"master", "M", "Master"},
    {"scrolling", "S", "Scrolling"},
    {"monocle", "Mo", "Monocle"},
};

char *wc_hypr_layout_json(const struct wc_hypr_layout *layout) {
  struct wc_buf json = {0};

  if (!layout || !layout->name[0]) {
    wc_buf_appendf(&json, "{\"text\":\"\",\"alt\":\"none\",\"class\":\"none\","
                          "\"tooltip\":\"Hyprland Not Running\"}");
    return wc_buf_finish(&json);
  }

  char letter[8], title[64];
  snprintf(letter, sizeof(letter), "%c", toupper((unsigned char)layout->name[0]));
  snprintf(title, sizeof(title), "%s", layout->name);
  title[0] = (char)toupper((unsigned char)title[0]);

  for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
    if (strcmp(layout->name, layouts[i].name) == 0) {
      snprintf(letter, sizeof(letter), "%s", layouts[i].letter);
      snprintf(title, sizeof(title), "%s", layouts[i].title);
    }
  }

  wc_buf_appendf(&json, "{\"text\":\"");
  wc_buf_append_text(&json, letter);
  wc_buf_appendf(&json, "\",\"alt\":\"");
  wc_buf_append_text(&json, layout->name);
  wc_buf_appendf(&json, "\",\"class\":\"");
  wc_buf_append_text(&json, layout->name);
  wc_buf_appendf(&json, "\",\"tooltip\":\"Active Layout: ");
  wc_buf_append_text(&json, title);
  wc_buf_appendf(&json, "\\nWorkspace: ");
  wc_buf_append_text(&json, layout->workspace_name);
  wc_buf_appendf(&json, "\"}");
  return wc_buf_finish(&json);
}

int wc_hypr_layout_watch(wc_json_cb cb, void *userdata) {
  char *last = NULL;
  int stop = 0;
  int events = -1;

  while (!stop) {
    struct wc_hypr_layout layout;
    char *json = wc_hypr_layout_json(
        wc_hypr_layout_get(&layout) == 0 ? &layout : NULL);
    if (json && (!last || strcmp(json, last) != 0)) {
      stop = cb(json, userdata);
      free(last);
      last = json;
    } else {
      free(json);
    }
    if (stop)
      break;

    if (events < 0)
      events = hypr_connect(".socket2.sock");

    /* Workspace and monitor switches arrive as events; layout changes made
     * by workspace rules at runtime do not, so also poll. */
    struct pollfd pfd = {.fd = events, .events = POLLIN};
    int r = poll(&pfd, events >= 0 ? 1 : 0, POLL_MS);
    if (r > 0) {
      char buf[8192];
      ssize_t n = read(events, buf, sizeof(buf));
      if (n <= 0) {
        close(events); // Hyprland restarted, reconnect
        events = -1;
      }
      wc_sleep_ms(20); // let a burst of events settle
    }
  }

  if (events >= 0)
    close(events);
  free(last);
  return 0;
}
