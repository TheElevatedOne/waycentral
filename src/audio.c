#include "../waycentral.h"
#include "util.h"
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_DEVICES 64

struct pa {
  pa_mainloop *ml;
  pa_context *ctx;
  enum wc_audio_dir dir;
  char default_name[256];
  struct wc_audio *out;
  /* device list for WC_AUDIO_NEXT_DEVICE */
  int device_count;
  char devices[MAX_DEVICES][256];
  int dirty;
};

static void pa_disconnect(struct pa *p) {
  if (p->ctx) {
    pa_context_disconnect(p->ctx);
    pa_context_unref(p->ctx);
    p->ctx = NULL;
  }
  if (p->ml) {
    pa_mainloop_free(p->ml);
    p->ml = NULL;
  }
}

static int pa_connect(struct pa *p) {
  p->ml = pa_mainloop_new();
  if (!p->ml)
    return -1;

  pa_proplist *props = pa_proplist_new();
  pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "waycentral");
  p->ctx = pa_context_new_with_proplist(pa_mainloop_get_api(p->ml),
                                        "waycentral", props);
  pa_proplist_free(props);

  if (!p->ctx ||
      pa_context_connect(p->ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) {
    pa_disconnect(p);
    return -1;
  }

  for (;;) {
    pa_context_state_t state = pa_context_get_state(p->ctx);
    if (state == PA_CONTEXT_READY)
      return 0;
    if (!PA_CONTEXT_IS_GOOD(state) ||
        pa_mainloop_iterate(p->ml, 1, NULL) < 0) {
      pa_disconnect(p);
      return -1;
    }
  }
}

/* Runs the mainloop until op is done. Returns 0, or -1 on error. */
static int pa_wait(struct pa *p, pa_operation *op) {
  if (!op)
    return -1;
  while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
    if (pa_mainloop_iterate(p->ml, 1, NULL) < 0) {
      pa_operation_cancel(op);
      pa_operation_unref(op);
      return -1;
    }
  }
  pa_operation_unref(op);
  return 0;
}

static void server_cb(pa_context *c, const pa_server_info *i, void *ud) {
  (void)c;
  struct pa *p = ud;
  const char *name = (p->dir == WC_AUDIO_OUTPUT) ? i->default_sink_name
                                                 : i->default_source_name;
  snprintf(p->default_name, sizeof(p->default_name), "%s", name ? name : "");
}

static enum wc_audio_form form_from_port(pa_device_port_type_t type) {
  switch (type) {
  case PA_DEVICE_PORT_TYPE_HEADPHONES:
    return WC_FORM_HEADPHONES;
  case PA_DEVICE_PORT_TYPE_HEADSET:
  case PA_DEVICE_PORT_TYPE_HANDSET:
  case PA_DEVICE_PORT_TYPE_EARPIECE:
    return WC_FORM_HEADSET;
  case PA_DEVICE_PORT_TYPE_SPEAKER:
    return WC_FORM_SPEAKER;
  case PA_DEVICE_PORT_TYPE_MIC:
    return WC_FORM_MICROPHONE;
  case PA_DEVICE_PORT_TYPE_HDMI:
  case PA_DEVICE_PORT_TYPE_SPDIF:
    return WC_FORM_HDMI;
  default:
    return WC_FORM_UNKNOWN;
  }
}

static enum wc_audio_form form_from_props(pa_proplist *props,
                                          const char *port_name) {
  const char *ff = pa_proplist_gets(props, PA_PROP_DEVICE_FORM_FACTOR);
  if (ff) {
    if (strcmp(ff, "headphone") == 0)
      return WC_FORM_HEADPHONES;
    if (strcmp(ff, "headset") == 0 || strcmp(ff, "handset") == 0 ||
        strcmp(ff, "hands-free") == 0)
      return WC_FORM_HEADSET;
    if (strcmp(ff, "speaker") == 0)
      return WC_FORM_SPEAKER;
    if (strcmp(ff, "microphone") == 0 || strcmp(ff, "webcam") == 0)
      return WC_FORM_MICROPHONE;
  }
  if (port_name) {
    if (strcasestr(port_name, "headphone"))
      return WC_FORM_HEADPHONES;
    if (strcasestr(port_name, "headset"))
      return WC_FORM_HEADSET;
    if (strcasestr(port_name, "hdmi"))
      return WC_FORM_HDMI;
  }
  return WC_FORM_UNKNOWN;
}

static void fill(struct pa *p, const char *name, const char *desc,
                 const pa_cvolume *vol, int mute, pa_proplist *props,
                 const char *port_name, const char *port_desc,
                 pa_device_port_type_t port_type) {
  struct wc_audio *a = p->out;
  memset(a, 0, sizeof(*a));
  a->available = 1;
  a->muted = mute;
  a->volume = (int)((pa_cvolume_avg(vol) * 100ULL + PA_VOLUME_NORM / 2) /
                    PA_VOLUME_NORM);
  snprintf(a->name, sizeof(a->name), "%s", name);
  snprintf(a->description, sizeof(a->description), "%s", desc ? desc : name);
  if (port_desc)
    snprintf(a->port, sizeof(a->port), "%s", port_desc);

  const char *bus = pa_proplist_gets(props, PA_PROP_DEVICE_BUS);
  a->bluetooth = bus && strcmp(bus, "bluetooth") == 0;

  a->form = port_name ? form_from_port(port_type) : WC_FORM_UNKNOWN;
  if (a->form == WC_FORM_UNKNOWN)
    a->form = form_from_props(props, port_name);
  if (a->form == WC_FORM_UNKNOWN)
    a->form = (p->dir == WC_AUDIO_OUTPUT) ? WC_FORM_SPEAKER
                                          : WC_FORM_MICROPHONE;
}

static void sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud) {
  (void)c;
  if (eol || !i)
    return;
  const pa_sink_port_info *port = i->active_port;
  fill(ud, i->name, i->description, &i->volume, i->mute, i->proplist,
       port ? port->name : NULL, port ? port->description : NULL,
       port ? port->type : PA_DEVICE_PORT_TYPE_UNKNOWN);
}

static void source_cb(pa_context *c, const pa_source_info *i, int eol,
                      void *ud) {
  (void)c;
  if (eol || !i)
    return;
  const pa_source_port_info *port = i->active_port;
  fill(ud, i->name, i->description, &i->volume, i->mute, i->proplist,
       port ? port->name : NULL, port ? port->description : NULL,
       port ? port->type : PA_DEVICE_PORT_TYPE_UNKNOWN);
}

static int query(struct pa *p, struct wc_audio *out) {
  memset(out, 0, sizeof(*out));
  p->out = out;
  p->default_name[0] = '\0';

  if (pa_wait(p, pa_context_get_server_info(p->ctx, server_cb, p)) != 0)
    return -1;
  if (!p->default_name[0])
    return 0; // no device

  pa_operation *op =
      (p->dir == WC_AUDIO_OUTPUT)
          ? pa_context_get_sink_info_by_name(p->ctx, p->default_name, sink_cb,
                                             p)
          : pa_context_get_source_info_by_name(p->ctx, p->default_name,
                                               source_cb, p);
  return pa_wait(p, op);
}

int wc_audio_get(enum wc_audio_dir dir, struct wc_audio *out) {
  struct pa p = {.dir = dir};
  if (pa_connect(&p) != 0)
    return -1;
  int r = query(&p, out);
  pa_disconnect(&p);
  return r;
}

static const char *form_name(enum wc_audio_form form) {
  switch (form) {
  case WC_FORM_SPEAKER:
    return "speaker";
  case WC_FORM_HEADPHONES:
    return "headphones";
  case WC_FORM_HEADSET:
    return "headset";
  case WC_FORM_MICROPHONE:
    return "microphone";
  case WC_FORM_HDMI:
    return "hdmi";
  default:
    return "unknown";
  }
}

char *wc_audio_json(const struct wc_audio *a, enum wc_audio_dir dir) {
  struct wc_buf json = {0};
  const char *what = (dir == WC_AUDIO_OUTPUT) ? "Output" : "Input";

  if (!a || !a->available) {
    wc_buf_appendf(&json,
                   "{\"text\":\"\",\"alt\":\"none\",\"class\":\"none\","
                   "\"tooltip\":\"No %s Device\"}",
                   what);
    return wc_buf_finish(&json);
  }

  const char *form = form_name(a->form);
  wc_buf_appendf(&json,
                 "{\"text\":\"%d%%\",\"percentage\":%d,\"alt\":\"%s%s\","
                 "\"class\":[\"%s\"%s],\"tooltip\":\"<b>%s:</b>\\t",
                 a->volume, a->volume, form, a->muted ? "-muted" : "", form,
                 a->muted ? ",\"muted\"" : "", what);
  wc_buf_append_text(&json, a->description);
  if (a->port[0]) {
    wc_buf_appendf(&json, "\\n<b>Port:</b>\\t");
    wc_buf_append_text(&json, a->port);
  }
  wc_buf_appendf(&json, "\\n<b>Volume:</b>\\t%d%%%s%s\"}", a->volume,
                 a->muted ? " (Muted)" : "",
                 a->bluetooth ? "\\n<b>Bluetooth</b>" : "");
  return wc_buf_finish(&json);
}

static void subscribe_cb(pa_context *c, pa_subscription_event_type_t t,
                         uint32_t idx, void *ud) {
  (void)c;
  (void)t;
  (void)idx;
  ((struct pa *)ud)->dirty = 1;
}

int wc_audio_watch(enum wc_audio_dir dir, wc_json_cb cb, void *userdata) {
  char *last = NULL;
  int stop = 0;

  while (!stop) {
    struct pa p = {.dir = dir};
    struct wc_audio a;
    char *json;

    if (pa_connect(&p) != 0) {
      // Sound server not up (yet), report and retry
      json = wc_audio_json(NULL, dir);
      if (json && (!last || strcmp(json, last) != 0)) {
        stop = cb(json, userdata);
        free(last);
        last = json;
      } else {
        free(json);
      }
      wc_sleep_ms(2000);
      continue;
    }

    pa_context_set_subscribe_callback(p.ctx, subscribe_cb, &p);
    pa_operation *op = pa_context_subscribe(
        p.ctx,
        PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
            PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_CARD,
        NULL, NULL);
    if (op)
      pa_operation_unref(op);

    p.dirty = 1;
    while (!stop) {
      if (p.dirty) {
        p.dirty = 0;
        if (query(&p, &a) != 0)
          break;
        json = wc_audio_json(&a, dir);
        if (json && (!last || strcmp(json, last) != 0)) {
          stop = cb(json, userdata);
          free(last);
          last = json;
        } else {
          free(json);
        }
      }
      if (pa_mainloop_iterate(p.ml, 1, NULL) < 0 ||
          !PA_CONTEXT_IS_GOOD(pa_context_get_state(p.ctx)))
        break; // server went away, reconnect
    }

    pa_disconnect(&p);
  }

  free(last);
  return 0;
}

static void list_sink_cb(pa_context *c, const pa_sink_info *i, int eol,
                         void *ud) {
  (void)c;
  struct pa *p = ud;
  if (eol || !i || p->device_count >= MAX_DEVICES)
    return;
  snprintf(p->devices[p->device_count++], sizeof(p->devices[0]), "%s",
           i->name);
}

static void list_source_cb(pa_context *c, const pa_source_info *i, int eol,
                           void *ud) {
  (void)c;
  struct pa *p = ud;
  // Skip the monitors of outputs, they are not microphones
  if (eol || !i || i->monitor_of_sink != PA_INVALID_INDEX ||
      p->device_count >= MAX_DEVICES)
    return;
  snprintf(p->devices[p->device_count++], sizeof(p->devices[0]), "%s",
           i->name);
}

static int next_device(struct pa *p, const char *current) {
  p->device_count = 0;
  pa_operation *op = (p->dir == WC_AUDIO_OUTPUT)
                         ? pa_context_get_sink_info_list(p->ctx,
                                                         list_sink_cb, p)
                         : pa_context_get_source_info_list(p->ctx,
                                                           list_source_cb, p);
  if (pa_wait(p, op) != 0 || p->device_count == 0)
    return -1;

  int next = 0;
  for (int i = 0; i < p->device_count; i++)
    if (strcmp(p->devices[i], current) == 0)
      next = (i + 1) % p->device_count;

  op = (p->dir == WC_AUDIO_OUTPUT)
           ? pa_context_set_default_sink(p->ctx, p->devices[next], NULL, NULL)
           : pa_context_set_default_source(p->ctx, p->devices[next], NULL,
                                           NULL);
  return pa_wait(p, op);
}

/* Volume info needed to change the volume while keeping the balance */
struct vol_info {
  pa_cvolume volume;
  int mute;
  int found;
};

static void vol_sink_cb(pa_context *c, const pa_sink_info *i, int eol,
                        void *ud) {
  (void)c;
  struct vol_info *v = ud;
  if (eol || !i)
    return;
  v->volume = i->volume;
  v->mute = i->mute;
  v->found = 1;
}

static void vol_source_cb(pa_context *c, const pa_source_info *i, int eol,
                          void *ud) {
  (void)c;
  struct vol_info *v = ud;
  if (eol || !i)
    return;
  v->volume = i->volume;
  v->mute = i->mute;
  v->found = 1;
}

int wc_audio_control(enum wc_audio_dir dir, enum wc_audio_action action,
                     int value) {
  struct pa p = {.dir = dir};
  if (pa_connect(&p) != 0)
    return -1;

  int r = -1;
  if (pa_wait(&p, pa_context_get_server_info(p.ctx, server_cb, &p)) != 0 ||
      !p.default_name[0])
    goto out;

  if (action == WC_AUDIO_NEXT_DEVICE) {
    r = next_device(&p, p.default_name);
    goto out;
  }

  struct vol_info v = {0};
  pa_operation *op =
      (dir == WC_AUDIO_OUTPUT)
          ? pa_context_get_sink_info_by_name(p.ctx, p.default_name,
                                             vol_sink_cb, &v)
          : pa_context_get_source_info_by_name(p.ctx, p.default_name,
                                               vol_source_cb, &v);
  if (pa_wait(&p, op) != 0 || !v.found)
    goto out;

  if (action == WC_AUDIO_MUTE_TOGGLE || action == WC_AUDIO_MUTE ||
      action == WC_AUDIO_UNMUTE) {
    int mute = (action == WC_AUDIO_MUTE_TOGGLE) ? !v.mute
               : (action == WC_AUDIO_MUTE)      ? 1
                                                : 0;
    op = (dir == WC_AUDIO_OUTPUT)
             ? pa_context_set_sink_mute_by_name(p.ctx, p.default_name, mute,
                                                NULL, NULL)
             : pa_context_set_source_mute_by_name(p.ctx, p.default_name, mute,
                                                  NULL, NULL);
    r = pa_wait(&p, op);
    goto out;
  }

  long current = (long)pa_cvolume_avg(&v.volume);
  long step = (long)value * PA_VOLUME_NORM / 100;
  long target = current;
  if (action == WC_AUDIO_SET)
    target = step;
  else if (action == WC_AUDIO_RAISE)
    target = (current >= (long)PA_VOLUME_NORM)
                 ? current
                 : (current + step > (long)PA_VOLUME_NORM ? (long)PA_VOLUME_NORM
                                                          : current + step);
  else if (action == WC_AUDIO_LOWER)
    target = current - step;

  if (target < 0)
    target = 0;
  if (target > (long)PA_VOLUME_MAX)
    target = (long)PA_VOLUME_MAX;

  if (current == 0)
    pa_cvolume_set(&v.volume, v.volume.channels, (pa_volume_t)target);
  else
    pa_cvolume_scale(&v.volume, (pa_volume_t)target);

  op = (dir == WC_AUDIO_OUTPUT)
           ? pa_context_set_sink_volume_by_name(p.ctx, p.default_name,
                                                &v.volume, NULL, NULL)
           : pa_context_set_source_volume_by_name(p.ctx, p.default_name,
                                                  &v.volume, NULL, NULL);
  r = pa_wait(&p, op);

out:
  pa_disconnect(&p);
  return r;
}
