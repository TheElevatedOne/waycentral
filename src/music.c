#define _GNU_SOURCE
#include "../waycentral.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_IFACE "org.mpris.MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"

static enum wc_music_status parse_status(const char *s) {
  if (!s)
    return WC_MUSIC_STOPPED;
  if (strcmp(s, "Playing") == 0)
    return WC_MUSIC_PLAYING;
  if (strcmp(s, "Paused") == 0)
    return WC_MUSIC_PAUSED;
  return WC_MUSIC_STOPPED;
}

static enum wc_music_status player_status(sd_bus *bus, const char *name) {
  char *s = NULL;
  sd_bus_get_property_string(bus, name, MPRIS_PATH, PLAYER_IFACE,
                             "PlaybackStatus", NULL, &s);
  enum wc_music_status status = parse_status(s);
  free(s);
  return status;
}

/* Picks the bus name of the player to follow into out.
 * Returns 0, or -1 if there is no matching player. */
static int pick_player(sd_bus *bus, const char *preferred, char *out,
                       size_t size) {
  char **names = NULL;
  if (sd_bus_list_names(bus, &names, NULL) < 0)
    return -1;

  const size_t plen = strlen(MPRIS_PREFIX);
  int best = -1;
  enum wc_music_status best_status = WC_MUSIC_NONE;

  for (int i = 0; names && names[i]; i++) {
    if (strncmp(names[i], MPRIS_PREFIX, plen) != 0)
      continue;
    const char *suffix = names[i] + plen;

    if (preferred && *preferred) {
      if (strncmp(suffix, preferred, strlen(preferred)) != 0)
        continue;
    } else if (strncmp(suffix, "playerctld", 10) == 0) {
      continue; // proxy for the other players, would show them twice
    }

    enum wc_music_status status = player_status(bus, names[i]);
    if (best < 0 || status > best_status) {
      best = i;
      best_status = status;
    }
  }

  int r = -1;
  if (best >= 0) {
    snprintf(out, size, "%s", names[best]);
    r = 0;
  }

  for (int i = 0; names && names[i]; i++)
    free(names[i]);
  free(names);
  return r;
}

/* Reads a variant holding a string or a string array (xesam:artist) */
static void read_text_variant(sd_bus_message *m, const char *contents,
                              char *out, size_t size) {
  if (strcmp(contents, "s") == 0) {
    const char *s;
    if (sd_bus_message_read(m, "s", &s) > 0)
      snprintf(out, size, "%s", s);
  } else if (strcmp(contents, "as") == 0) {
    char **list = NULL;
    if (sd_bus_message_read_strv(m, &list) >= 0 && list) {
      size_t len = 0;
      out[0] = '\0';
      for (int i = 0; list[i]; i++) {
        int n = snprintf(out + len, size - len, "%s%s", i ? ", " : "",
                         list[i]);
        if (n > 0 && (size_t)n < size - len)
          len += (size_t)n;
        free(list[i]);
      }
      free(list);
    }
  } else {
    sd_bus_message_skip(m, contents);
  }
}

static long long read_int_variant(sd_bus_message *m, const char *contents) {
  if (strcmp(contents, "x") == 0) {
    int64_t v;
    return sd_bus_message_read(m, "x", &v) > 0 ? (long long)v : -1;
  }
  if (strcmp(contents, "t") == 0) {
    uint64_t v;
    return sd_bus_message_read(m, "t", &v) > 0 ? (long long)v : -1;
  }
  if (strcmp(contents, "i") == 0) {
    int32_t v;
    return sd_bus_message_read(m, "i", &v) > 0 ? (long long)v : -1;
  }
  if (strcmp(contents, "u") == 0) {
    uint32_t v;
    return sd_bus_message_read(m, "u", &v) > 0 ? (long long)v : -1;
  }
  sd_bus_message_skip(m, contents);
  return -1;
}

static void read_metadata(sd_bus *bus, const char *name, struct wc_music *out) {
  sd_bus_message *reply = NULL;
  if (sd_bus_get_property(bus, name, MPRIS_PATH, PLAYER_IFACE, "Metadata",
                          NULL, &reply, "a{sv}") < 0)
    return;

  if (sd_bus_message_enter_container(reply, 'a', "{sv}") <= 0)
    goto out;

  while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
    const char *key, *contents;
    if (sd_bus_message_read(reply, "s", &key) <= 0 ||
        sd_bus_message_peek_type(reply, NULL, &contents) <= 0 ||
        sd_bus_message_enter_container(reply, 'v', contents) <= 0)
      break;

    if (strcmp(key, "xesam:title") == 0)
      read_text_variant(reply, contents, out->title, sizeof(out->title));
    else if (strcmp(key, "xesam:artist") == 0)
      read_text_variant(reply, contents, out->artist, sizeof(out->artist));
    else if (strcmp(key, "xesam:album") == 0)
      read_text_variant(reply, contents, out->album, sizeof(out->album));
    else if (strcmp(key, "mpris:length") == 0)
      out->length = read_int_variant(reply, contents);
    else
      sd_bus_message_skip(reply, contents);

    sd_bus_message_exit_container(reply); // v
    sd_bus_message_exit_container(reply); // e
  }

out:
  sd_bus_message_unref(reply);
}

static int get_with_bus(sd_bus *bus, const char *preferred,
                        struct wc_music *out) {
  memset(out, 0, sizeof(*out));
  out->position = -1;
  out->length = -1;

  char name[256];
  if (pick_player(bus, preferred, name, sizeof(name)) != 0)
    return 0; // WC_MUSIC_NONE

  snprintf(out->player, sizeof(out->player), "%s",
           name + strlen(MPRIS_PREFIX));
  out->status = player_status(bus, name);

  char *identity = NULL;
  if (sd_bus_get_property_string(bus, name, MPRIS_PATH, MPRIS_IFACE,
                                 "Identity", NULL, &identity) >= 0)
    snprintf(out->identity, sizeof(out->identity), "%s", identity);
  else
    snprintf(out->identity, sizeof(out->identity), "%s", out->player);
  free(identity);

  read_metadata(bus, name, out);

  int64_t pos;
  if (sd_bus_get_property_trivial(bus, name, MPRIS_PATH, PLAYER_IFACE,
                                  "Position", NULL, 'x', &pos) >= 0)
    out->position = (long long)pos;

  return 0;
}

int wc_music_get(const char *preferred, struct wc_music *out) {
  sd_bus *bus = NULL;
  if (sd_bus_open_user(&bus) < 0)
    return -1;
  int r = get_with_bus(bus, preferred, out);
  sd_bus_flush_close_unref(bus);
  return r;
}

/* Copies at most max UTF-8 characters of src, adding "…" when cut */
static void utf8_cut(char *out, size_t size, const char *src, int max) {
  if (max <= 0) {
    snprintf(out, size, "%s", src);
    return;
  }

  int chars = 0;
  size_t i = 0;
  while (src[i] && chars < max) {
    i++;
    while ((src[i] & 0xC0) == 0x80) // continuation bytes
      i++;
    chars++;
  }
  if (src[i])
    snprintf(out, size, "%.*s…", (int)i, src);
  else
    snprintf(out, size, "%s", src);
}

static void format_time(char *out, size_t size, long long us) {
  if (us < 0) {
    snprintf(out, size, "--:--");
    return;
  }
  long long s = us / 1000000;
  if (s >= 3600)
    snprintf(out, size, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
  else
    snprintf(out, size, "%lld:%02lld", s / 60, s % 60);
}

static const char *status_name(enum wc_music_status status) {
  switch (status) {
  case WC_MUSIC_PLAYING:
    return "playing";
  case WC_MUSIC_PAUSED:
    return "paused";
  case WC_MUSIC_STOPPED:
    return "stopped";
  default:
    return "none";
  }
}

char *wc_music_json(const struct wc_music *m, int max_length) {
  struct wc_buf json = {0};

  if (!m || m->status == WC_MUSIC_NONE) {
    wc_buf_appendf(&json, "{\"text\":\"\",\"alt\":\"none\",\"class\":\"none\","
                          "\"tooltip\":\"No Player Running\"}");
    return wc_buf_finish(&json);
  }

  char full[1100], text[1100];
  if (m->artist[0] && m->title[0])
    snprintf(full, sizeof(full), "%s - %s", m->artist, m->title);
  else
    snprintf(full, sizeof(full), "%s", m->title[0] ? m->title : m->identity);
  utf8_cut(text, sizeof(text), full, max_length);

  const char *status = status_name(m->status);
  wc_buf_appendf(&json, "{\"text\":\"");
  wc_buf_append_text(&json, text);
  wc_buf_appendf(&json,
                 "\",\"alt\":\"%s\",\"class\":\"%s\",\"tooltip\":\"<b>Player:"
                 "</b>\\t",
                 status, status);
  wc_buf_append_text(&json, m->identity);
  if (m->title[0]) {
    wc_buf_appendf(&json, "\\n<b>Title:</b>\\t");
    wc_buf_append_text(&json, m->title);
  }
  if (m->artist[0]) {
    wc_buf_appendf(&json, "\\n<b>Artist:</b>\\t");
    wc_buf_append_text(&json, m->artist);
  }
  if (m->album[0]) {
    wc_buf_appendf(&json, "\\n<b>Album:</b>\\t");
    wc_buf_append_text(&json, m->album);
  }
  if (m->length > 0) {
    char pos[32], len[32];
    format_time(pos, sizeof(pos), m->position);
    format_time(len, sizeof(len), m->length);
    wc_buf_appendf(&json, "\\n<b>Time:</b>\\t%s / %s", pos, len);
  }
  wc_buf_appendf(&json, "\\n<b>Status:</b>\\t%c%s\"}", status[0] - 32,
                 status + 1);
  return wc_buf_finish(&json);
}

static int on_signal(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)m;
  (void)err;
  *(int *)userdata = 1;
  return 0;
}

int wc_music_watch(const char *preferred, int max_length, wc_json_cb cb,
                   void *userdata) {
  sd_bus *bus = NULL;
  if (sd_bus_open_user(&bus) < 0)
    return -1;

  int dirty = 1;
  sd_bus_add_match(bus, NULL,
                   "type='signal',interface='org.freedesktop.DBus."
                   "Properties',member='PropertiesChanged',path='" MPRIS_PATH
                   "'",
                   on_signal, &dirty);
  sd_bus_add_match(bus, NULL,
                   "type='signal',interface='" PLAYER_IFACE
                   "',member='Seeked',path='" MPRIS_PATH "'",
                   on_signal, &dirty);
  sd_bus_add_match(bus, NULL,
                   "type='signal',sender='org.freedesktop.DBus',member='"
                   "NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'",
                   on_signal, &dirty);

  char *last = NULL;
  int stop = 0, ret = 0;
  enum wc_music_status status = WC_MUSIC_NONE;

  while (!stop) {
    if (dirty) {
      dirty = 0;
      struct wc_music m;
      get_with_bus(bus, preferred, &m);
      status = m.status;
      char *json = wc_music_json(&m, max_length);
      if (json && (!last || strcmp(json, last) != 0)) {
        stop = cb(json, userdata);
        free(last);
        last = json;
      } else {
        free(json);
      }
      continue;
    }

    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0)
      ;
    if (r < 0) {
      ret = -1;
      break;
    }
    if (dirty)
      continue;

    // While playing, refresh every second for the position
    uint64_t timeout = (status == WC_MUSIC_PLAYING) ? 1000000 : UINT64_MAX;
    r = sd_bus_wait(bus, timeout);
    if (r == 0)
      dirty = 1;
    else if (r < 0 && r != -EINTR) {
      ret = -1;
      break;
    }
  }

  free(last);
  sd_bus_flush_close_unref(bus);
  return ret;
}

int wc_music_control(const char *preferred, enum wc_music_action action) {
  static const char *methods[] = {"PlayPause", "Play", "Pause", "Stop",
                                  "Next",      "Previous", "Raise"};
  if (action < WC_MUSIC_PLAY_PAUSE || action > WC_MUSIC_RAISE)
    return -1;

  sd_bus *bus = NULL;
  if (sd_bus_open_user(&bus) < 0)
    return -1;

  int r = -1;
  char name[256];
  if (pick_player(bus, preferred, name, sizeof(name)) == 0) {
    const char *iface = (action == WC_MUSIC_RAISE) ? MPRIS_IFACE : PLAYER_IFACE;
    r = sd_bus_call_method(bus, name, MPRIS_PATH, iface, methods[action],
                           NULL, NULL, "") >= 0
            ? 0
            : -1;
  }

  sd_bus_flush_close_unref(bus);
  return r;
}

int wc_music_launch(const char *binary) {
  if (!binary || !*binary)
    return -1;

  // The pipe reports an exec failure; it closes silently on success
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) != 0)
    return -1;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    close(pipefd[0]);
    setsid();
    // Fork again so the player is not our child and leaves no zombie
    if (fork() != 0)
      _exit(0);

    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      dup2(null, STDOUT_FILENO);
      dup2(null, STDERR_FILENO);
      if (null > STDERR_FILENO)
        close(null);
    }
    execlp(binary, binary, (char *)NULL);
    int e = errno;
    if (write(pipefd[1], &e, sizeof(e)) < 0) {
    }
    _exit(127);
  }

  close(pipefd[1]);
  waitpid(pid, NULL, 0);

  int e = 0;
  ssize_t n;
  while ((n = read(pipefd[0], &e, sizeof(e))) < 0 && errno == EINTR)
    ;
  close(pipefd[0]);
  return n > 0 ? -1 : 0;
}
