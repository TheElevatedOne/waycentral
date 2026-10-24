#define _GNU_SOURCE
#include "../waycentral.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define REFRESH_MS 5000

/* ---- small sd-bus property helpers ---- */

static int get_u32(sd_bus *bus, const char *path, const char *iface,
                   const char *prop, uint32_t *out) {
  return sd_bus_get_property_trivial(bus, NM, path, iface, prop, NULL, 'u',
                                     out);
}

static int get_bool(sd_bus *bus, const char *path, const char *iface,
                    const char *prop, int *out) {
  return sd_bus_get_property_trivial(bus, NM, path, iface, prop, NULL, 'b',
                                     out);
}

static int get_str(sd_bus *bus, const char *path, const char *iface,
                   const char *prop, char *out, size_t size) {
  char *s = NULL;
  int r = sd_bus_get_property_string(bus, NM, path, iface, prop, NULL, &s);
  if (r >= 0)
    snprintf(out, size, "%s", s);
  free(s);
  return r;
}

static int get_path(sd_bus *bus, const char *path, const char *iface,
                    const char *prop, char *out, size_t size) {
  sd_bus_message *reply = NULL;
  const char *p = NULL;
  int r = sd_bus_get_property(bus, NM, path, iface, prop, NULL, &reply, "o");
  if (r >= 0)
    r = sd_bus_message_read(reply, "o", &p);
  if (r >= 0)
    snprintf(out, size, "%s", p);
  sd_bus_message_unref(reply);
  return r;
}

static int get_first_path(sd_bus *bus, const char *path, const char *iface,
                          const char *prop, char *out, size_t size) {
  sd_bus_message *reply = NULL;
  const char *p = NULL;
  int r = sd_bus_get_property(bus, NM, path, iface, prop, NULL, &reply, "ao");
  if (r >= 0)
    r = sd_bus_message_enter_container(reply, 'a', "o");
  if (r >= 0)
    r = sd_bus_message_read(reply, "o", &p);
  if (r > 0)
    snprintf(out, size, "%s", p);
  else
    r = -1;
  sd_bus_message_unref(reply);
  return r;
}

static int get_ssid(sd_bus *bus, const char *ap, char *out, size_t size) {
  sd_bus_message *reply = NULL;
  const void *data = NULL;
  size_t len = 0;
  int r = sd_bus_get_property(bus, NM, ap, NM ".AccessPoint", "Ssid", NULL,
                              &reply, "ay");
  if (r >= 0)
    r = sd_bus_message_read_array(reply, 'y', &data, &len);
  if (r >= 0)
    snprintf(out, size, "%.*s", (int)len, (const char *)data);
  sd_bus_message_unref(reply);
  return r;
}

/* ---- kernel side: addresses, gateway, fallback ---- */

static void fill_addresses(struct wc_network *net) {
  struct ifaddrs *ifa_list;
  if (!net->iface[0] || getifaddrs(&ifa_list) != 0)
    return;

  for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || strcmp(ifa->ifa_name, net->iface) != 0)
      continue;

    if (ifa->ifa_addr->sa_family == AF_INET && !net->ipv4[0]) {
      struct sockaddr_in *a = (struct sockaddr_in *)ifa->ifa_addr;
      inet_ntop(AF_INET, &a->sin_addr, net->ipv4, sizeof(net->ipv4));
      if (ifa->ifa_netmask) {
        uint32_t mask =
            ntohl(((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
        net->prefix4 = __builtin_popcount(mask);
      }
    } else if (ifa->ifa_addr->sa_family == AF_INET6 && !net->ipv6[0]) {
      struct sockaddr_in6 *a = (struct sockaddr_in6 *)ifa->ifa_addr;
      if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr))
        continue;
      inet_ntop(AF_INET6, &a->sin6_addr, net->ipv6, sizeof(net->ipv6));
    }
  }
  freeifaddrs(ifa_list);
}

/* Default route from /proc/net/route. Fills iface if it is empty and
 * returns 0 if a default route was found. */
static int default_route(struct wc_network *net) {
  FILE *f = fopen("/proc/net/route", "r");
  if (!f)
    return -1;

  char line[256], iface[64];
  unsigned int dest, gateway, flags;
  int found = -1;

  if (!fgets(line, sizeof(line), f)) { // header
    fclose(f);
    return -1;
  }
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "%63s %x %x %x", iface, &dest, &gateway, &flags) != 4)
      continue;
    if (dest != 0 || !(flags & 0x2)) // RTF_GATEWAY
      continue;
    if (net->iface[0] && strcmp(iface, net->iface) != 0)
      continue;

    if (!net->iface[0])
      snprintf(net->iface, sizeof(net->iface), "%s", iface);
    struct in_addr gw = {.s_addr = gateway};
    inet_ntop(AF_INET, &gw, net->gateway, sizeof(net->gateway));
    found = 0;
    break;
  }
  fclose(f);
  return found;
}

static int wireless_signal(const char *iface) {
  FILE *f = fopen("/proc/net/wireless", "r");
  if (!f)
    return -1;

  char line[256];
  int signal = -1;
  while (fgets(line, sizeof(line), f)) {
    char name[64];
    float link;
    if (sscanf(line, " %63[^:]: %*x %f", name, &link) == 2 &&
        strcmp(name, iface) == 0) {
      signal = (int)(link * 100 / 70);
      if (signal > 100)
        signal = 100;
    }
  }
  fclose(f);
  return signal;
}

static void fallback_get(struct wc_network *net) {
  if (default_route(net) != 0) {
    net->type = WC_NET_DISCONNECTED;
    return;
  }

  char path[128];
  snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", net->iface);
  if (access(path, F_OK) == 0) {
    net->type = WC_NET_WIFI;
    net->signal = wireless_signal(net->iface);
  } else {
    net->type = WC_NET_ETHERNET;
    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", net->iface);
    int speed;
    if (wc_read_int(path, &speed) == 0 && speed > 0)
      net->bitrate = speed;
  }
  snprintf(net->connection, sizeof(net->connection), "%s", net->iface);
  fill_addresses(net);
}

/* ---- NetworkManager ---- */

static enum wc_net_type type_from_nm(const char *type) {
  if (strcmp(type, "802-11-wireless") == 0)
    return WC_NET_WIFI;
  if (strcmp(type, "802-3-ethernet") == 0)
    return WC_NET_ETHERNET;
  if (strcmp(type, "vpn") == 0 || strcmp(type, "wireguard") == 0)
    return WC_NET_VPN;
  return WC_NET_OTHER;
}

static int nm_get(sd_bus *bus, struct wc_network *net) {
  int enabled = 1;
  uint32_t state = 0;
  if (get_bool(bus, NM_PATH, NM, "NetworkingEnabled", &enabled) < 0 ||
      get_u32(bus, NM_PATH, NM, "State", &state) < 0)
    return -1; // NetworkManager not running

  if (!enabled || state == 10) { // NM_STATE_ASLEEP
    net->type = WC_NET_DISABLED;
    return 0;
  }

  char conn[256];
  if (get_path(bus, NM_PATH, NM, "PrimaryConnection", conn, sizeof(conn)) <
          0 ||
      strcmp(conn, "/") == 0) {
    net->type = WC_NET_DISCONNECTED;
    return 0;
  }

  char type[64] = "";
  get_str(bus, conn, NM ".Connection.Active", "Id", net->connection,
          sizeof(net->connection));
  get_str(bus, conn, NM ".Connection.Active", "Type", type, sizeof(type));
  net->type = type_from_nm(type);

  char dev[256];
  if (get_first_path(bus, conn, NM ".Connection.Active", "Devices", dev,
                     sizeof(dev)) > 0) {
    if (get_str(bus, dev, NM ".Device", "IpInterface", net->iface,
                sizeof(net->iface)) < 0 ||
        !net->iface[0])
      get_str(bus, dev, NM ".Device", "Interface", net->iface,
              sizeof(net->iface));

    if (net->type == WC_NET_WIFI) {
      uint32_t bitrate = 0;
      if (get_u32(bus, dev, NM ".Device.Wireless", "Bitrate", &bitrate) >= 0)
        net->bitrate = (int)(bitrate / 1000);

      char ap[256];
      if (get_path(bus, dev, NM ".Device.Wireless", "ActiveAccessPoint", ap,
                   sizeof(ap)) >= 0 &&
          strcmp(ap, "/") != 0) {
        uint8_t strength = 0;
        uint32_t freq = 0;
        get_ssid(bus, ap, net->ssid, sizeof(net->ssid));
        if (sd_bus_get_property_trivial(bus, NM, ap, NM ".AccessPoint",
                                        "Strength", NULL, 'y',
                                        &strength) >= 0)
          net->signal = strength;
        if (get_u32(bus, ap, NM ".AccessPoint", "Frequency", &freq) >= 0)
          net->frequency = (int)freq;
      }
    } else if (net->type == WC_NET_ETHERNET) {
      uint32_t speed = 0;
      if (get_u32(bus, dev, NM ".Device.Wired", "Speed", &speed) >= 0)
        net->bitrate = (int)speed;
    }
  }

  default_route(net);
  fill_addresses(net);
  return 0;
}

static void get_with_bus(sd_bus *bus, struct wc_network *net) {
  memset(net, 0, sizeof(*net));
  net->signal = -1;
  if (!bus || nm_get(bus, net) != 0) {
    memset(net, 0, sizeof(*net));
    net->signal = -1;
    fallback_get(net);
  }
}

int wc_network_get(struct wc_network *out) {
  sd_bus *bus = NULL;
  if (sd_bus_open_system(&bus) < 0)
    bus = NULL;
  get_with_bus(bus, out);
  sd_bus_flush_close_unref(bus);
  return 0;
}

static const char *type_name(enum wc_net_type type) {
  switch (type) {
  case WC_NET_ETHERNET:
    return "ethernet";
  case WC_NET_WIFI:
    return "wifi";
  case WC_NET_VPN:
    return "vpn";
  case WC_NET_OTHER:
    return "other";
  case WC_NET_DISABLED:
    return "disabled";
  default:
    return "disconnected";
  }
}

char *wc_network_json(const struct wc_network *net) {
  struct wc_buf json = {0};
  const char *type = type_name(net->type);

  if (net->type == WC_NET_DISCONNECTED || net->type == WC_NET_DISABLED) {
    wc_buf_appendf(&json,
                   "{\"text\":\"%s\",\"alt\":\"%s\",\"class\":\"%s\","
                   "\"percentage\":0,\"tooltip\":\"%s\"}",
                   net->type == WC_NET_DISABLED ? "Disabled" : "Disconnected",
                   type, type,
                   net->type == WC_NET_DISABLED ? "Networking Disabled"
                                                : "Disconnected");
    return wc_buf_finish(&json);
  }

  const char *text = (net->type == WC_NET_WIFI && net->ssid[0]) ? net->ssid
                     : (net->type == WC_NET_VPN)               ? net->connection
                                                               : net->iface;

  wc_buf_appendf(&json, "{\"text\":\"");
  wc_buf_append_text(&json, text);
  wc_buf_appendf(&json,
                 "\",\"alt\":\"%s\",\"class\":\"%s\",\"percentage\":%d,"
                 "\"tooltip\":\"<b>Connection:</b>\\t",
                 type, type, net->signal >= 0 ? net->signal : 100);
  wc_buf_append_text(&json, net->connection);
  wc_buf_appendf(&json, "\\n<b>Interface:</b>\\t");
  wc_buf_append_text(&json, net->iface);

  if (net->type == WC_NET_WIFI) {
    if (net->ssid[0]) {
      wc_buf_appendf(&json, "\\n<b>SSID:</b>\\t");
      wc_buf_append_text(&json, net->ssid);
    }
    if (net->signal >= 0)
      wc_buf_appendf(&json, "\\n<b>Signal:</b>\\t%d%%", net->signal);
    if (net->frequency > 0)
      wc_buf_appendf(&json, "\\n<b>Freq:</b>\\t%.1f GHz",
                     net->frequency / 1000.0);
  }
  if (net->bitrate > 0)
    wc_buf_appendf(&json, "\\n<b>Speed:</b>\\t%d Mbit/s", net->bitrate);
  if (net->ipv4[0])
    wc_buf_appendf(&json, "\\n<b>IPv4:</b>\\t%s/%d", net->ipv4, net->prefix4);
  if (net->ipv6[0])
    wc_buf_appendf(&json, "\\n<b>IPv6:</b>\\t%s", net->ipv6);
  if (net->gateway[0])
    wc_buf_appendf(&json, "\\n<b>Gateway:</b>\\t%s", net->gateway);

  wc_buf_appendf(&json, "\"}");
  return wc_buf_finish(&json);
}

static int on_signal(sd_bus_message *m, void *userdata, sd_bus_error *err) {
  (void)m;
  (void)err;
  *(int *)userdata = 1;
  return 0;
}

int wc_network_watch(wc_json_cb cb, void *userdata) {
  sd_bus *bus = NULL;
  int dirty = 1;

  if (sd_bus_open_system(&bus) >= 0) {
    // Any property change of NetworkManager objects, and NM (re)starting
    sd_bus_match_signal(bus, NULL, NM, NULL, "org.freedesktop.DBus.Properties",
                        "PropertiesChanged", on_signal, &dirty);
    sd_bus_match_signal(bus, NULL, NM, NM_PATH, NM, "StateChanged", on_signal,
                        &dirty);
    sd_bus_add_match(bus, NULL,
                     "type='signal',sender='org.freedesktop.DBus',"
                     "member='NameOwnerChanged',arg0='" NM "'",
                     on_signal, &dirty);
  } else {
    bus = NULL;
  }

  char *last = NULL;
  int stop = 0, ret = 0;
  while (!stop) {
    if (dirty) {
      dirty = 0;
      struct wc_network net;
      get_with_bus(bus, &net);
      char *json = wc_network_json(&net);
      if (json && (!last || strcmp(json, last) != 0)) {
        stop = cb(json, userdata);
        free(last);
        last = json;
      } else {
        free(json);
      }
      continue;
    }

    if (!bus) {
      wc_sleep_ms(REFRESH_MS);
      dirty = 1;
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

    r = sd_bus_wait(bus, (uint64_t)REFRESH_MS * 1000);
    if (r == 0)
      dirty = 1; // periodic refresh for the signal strength
    else if (r < 0 && r != -EINTR) {
      ret = -1;
      break;
    }
  }

  free(last);
  sd_bus_flush_close_unref(bus);
  return ret;
}
