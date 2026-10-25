/*
 * waycentral-tray: a StatusNotifierItem system tray for Waybar, loaded as a
 * CFFI module. Icons come from the GTK icon theme (symbolic variants first,
 * recoloured with the theme's foreground colour), menus are built from the
 * items' com.canonical.dbusmenu layout every time they are opened.
 *
 * "cffi/tray": {
 *   "module_path": "/usr/lib/waycentral/waycentral-tray.so",
 *   "icon-size": 16, "spacing": 8,
 *   "prefer-symbolic": true, "show-passive": false
 * }
 */
#include "waybar_cffi_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define MENU_IFACE "com.canonical.dbusmenu"

const size_t wbcffi_version = 2;

typedef struct Tray Tray;

typedef struct {
  Tray *tray;
  char *id; /* "service/path" as registered with the watcher */
  char *service;
  char *path;
  GCancellable *cancel;
  GDBusProxy *proxy;
  GtkWidget *button;
  GtkWidget *image;
  GtkWidget *menu;
  double scroll_dx, scroll_dy;
} Item;

/* An item registered with our own watcher */
typedef struct {
  Tray *tray;
  char *id;
  guint name_watch;
} WatchedItem;

struct Tray {
  wbcffi_module *obj;
  GtkWidget *box;
  int icon_size;
  int spacing;
  gboolean prefer_symbolic;
  gboolean show_passive;

  GDBusConnection *bus;
  GCancellable *cancel; /* for calls that outlive nothing but the tray */
  GList *items;
  gboolean updating;

  /* watcher side */
  guint watcher_own;
  guint watcher_reg;
  GPtrArray *watched;

  /* host side */
  char *host_name;
  guint host_own;
  guint watcher_watch;
  guint sub_registered;
  guint sub_unregistered;
};

static void item_update(Item *item);
static void tray_add_item(Tray *tray, const char *id);
static void tray_remove_item(Tray *tray, const char *id);

/* ------------------------------------------------------------------------ */
/* Config                                                                   */
/* ------------------------------------------------------------------------ */

/* ABI 2 passes every value as JSON text */
static void parse_config(Tray *tray, const wbcffi_config_entry *entries,
                         size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char *k = entries[i].key, *v = entries[i].value;
    if (strcmp(k, "icon-size") == 0)
      tray->icon_size = atoi(v);
    else if (strcmp(k, "spacing") == 0)
      tray->spacing = atoi(v);
    else if (strcmp(k, "prefer-symbolic") == 0)
      tray->prefer_symbolic = strcmp(v, "true") == 0;
    else if (strcmp(k, "show-passive") == 0)
      tray->show_passive = strcmp(v, "true") == 0;
  }
  if (tray->icon_size <= 0)
    tray->icon_size = 16;
  if (tray->spacing < 0)
    tray->spacing = 0;
}

/* ------------------------------------------------------------------------ */
/* Icons                                                                    */
/* ------------------------------------------------------------------------ */

static gchar *prop_string(GDBusProxy *proxy, const char *name) {
  GVariant *v = g_dbus_proxy_get_cached_property(proxy, name);
  gchar *s = NULL;
  // Menu is an object path ("o"), the rest are strings
  if (v && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
            g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
    s = g_variant_dup_string(v, NULL);
  if (v)
    g_variant_unref(v);
  return s;
}

static gboolean prop_bool(GDBusProxy *proxy, const char *name) {
  GVariant *v = g_dbus_proxy_get_cached_property(proxy, name);
  gboolean b = FALSE;
  if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
    b = g_variant_get_boolean(v);
  if (v)
    g_variant_unref(v);
  return b;
}

static void add_theme_path(const char *path) {
  if (!path || !*path)
    return;
  GtkIconTheme *theme = gtk_icon_theme_get_default();
  gchar **paths = NULL;
  gint n = 0;
  gtk_icon_theme_get_search_path(theme, &paths, &n);
  gboolean found = FALSE;
  for (gint i = 0; i < n && !found; i++)
    found = strcmp(paths[i], path) == 0;
  g_strfreev(paths);
  if (!found)
    gtk_icon_theme_append_search_path(theme, path);
}

/* Loads name from the icon theme, recolouring symbolic icons */
static GdkPixbuf *load_theme_icon(Item *item, const char *name, int size,
                                  int scale) {
  GtkIconInfo *info = gtk_icon_theme_lookup_icon_for_scale(
      gtk_icon_theme_get_default(), name, size, scale,
      GTK_ICON_LOOKUP_FORCE_SIZE);
  if (!info)
    return NULL;

  GdkPixbuf *pixbuf;
  if (gtk_icon_info_is_symbolic(info))
    pixbuf = gtk_icon_info_load_symbolic_for_context(
        info, gtk_widget_get_style_context(item->image), NULL, NULL);
  else
    pixbuf = gtk_icon_info_load_icon(info, NULL);
  g_object_unref(info);
  return pixbuf;
}

static GdkPixbuf *load_named_icon(Item *item, const char *name, int size,
                                  int scale) {
  if (!name || !*name)
    return NULL;

  if (name[0] == '/')
    return gdk_pixbuf_new_from_file_at_size(name, size * scale, size * scale,
                                            NULL);

  gboolean is_symbolic = g_str_has_suffix(name, "-symbolic");
  gchar *symbolic = is_symbolic ? g_strdup(name)
                                : g_strconcat(name, "-symbolic", NULL);
  const char *order[2];
  if (item->tray->prefer_symbolic) {
    order[0] = symbolic;
    order[1] = name;
  } else {
    order[0] = name;
    order[1] = symbolic;
  }

  GdkPixbuf *pixbuf = NULL;
  for (int i = 0; i < 2 && !pixbuf; i++)
    pixbuf = load_theme_icon(item, order[i], size, scale);
  g_free(symbolic);
  return pixbuf;
}

static void free_pixels(guchar *pixels, gpointer data) {
  (void)data;
  g_free(pixels);
}

/* IconPixmap: a(iiay), ARGB32 in network byte order */
static GdkPixbuf *load_pixmap(GVariant *pixmaps, int px) {
  if (!pixmaps || !g_variant_is_of_type(pixmaps, G_VARIANT_TYPE("a(iiay)")))
    return NULL;

  GVariantIter iter;
  gint w, h, best_w = 0, best_h = 0;
  GVariant *data, *best = NULL;
  g_variant_iter_init(&iter, pixmaps);
  while (g_variant_iter_next(&iter, "(ii@ay)", &w, &h, &data)) {
    // Smallest one that is at least px, else the largest
    gboolean better = !best || (best_w < px && w > best_w) ||
                      (w >= px && w < best_w);
    if (better && w > 0 && h > 0 &&
        (gsize)w * h * 4 == g_variant_get_size(data)) {
      if (best)
        g_variant_unref(best);
      best = data;
      best_w = w;
      best_h = h;
    } else {
      g_variant_unref(data);
    }
  }
  if (!best)
    return NULL;

  gsize len = 0;
  const guchar *argb = g_variant_get_fixed_array(best, &len, 1);
  guchar *rgba = g_malloc(len);
  for (gsize i = 0; i + 3 < len; i += 4) {
    rgba[i] = argb[i + 1];
    rgba[i + 1] = argb[i + 2];
    rgba[i + 2] = argb[i + 3];
    rgba[i + 3] = argb[i];
  }
  g_variant_unref(best);

  GdkPixbuf *raw = gdk_pixbuf_new_from_data(rgba, GDK_COLORSPACE_RGB, TRUE, 8,
                                            best_w, best_h, best_w * 4,
                                            free_pixels, NULL);
  GdkPixbuf *scaled =
      gdk_pixbuf_scale_simple(raw, px, px, GDK_INTERP_BILINEAR);
  g_object_unref(raw);
  return scaled;
}

static void item_update_icon(Item *item) {
  GDBusProxy *p = item->proxy;
  int size = item->tray->icon_size;
  int scale = gtk_widget_get_scale_factor(item->image);

  gchar *theme_path = prop_string(p, "IconThemePath");
  add_theme_path(theme_path);
  g_free(theme_path);

  gchar *status = prop_string(p, "Status");
  gboolean attention = status && strcmp(status, "NeedsAttention") == 0;
  g_free(status);

  GdkPixbuf *pixbuf = NULL;
  if (attention) {
    gchar *name = prop_string(p, "AttentionIconName");
    pixbuf = load_named_icon(item, name, size, scale);
    g_free(name);
    if (!pixbuf) {
      GVariant *pm = g_dbus_proxy_get_cached_property(p, "AttentionIconPixmap");
      pixbuf = load_pixmap(pm, size * scale);
      if (pm)
        g_variant_unref(pm);
    }
  }
  if (!pixbuf) {
    gchar *name = prop_string(p, "IconName");
    pixbuf = load_named_icon(item, name, size, scale);
    g_free(name);
  }
  if (!pixbuf) {
    GVariant *pm = g_dbus_proxy_get_cached_property(p, "IconPixmap");
    pixbuf = load_pixmap(pm, size * scale);
    if (pm)
      g_variant_unref(pm);
  }
  if (!pixbuf)
    pixbuf = load_named_icon(item, "image-missing", size, scale);

  if (pixbuf) {
    cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf(
        pixbuf, scale, gtk_widget_get_window(item->image));
    gtk_image_set_from_surface(GTK_IMAGE(item->image), surface);
    cairo_surface_destroy(surface);
    g_object_unref(pixbuf);
  } else {
    gtk_image_clear(GTK_IMAGE(item->image));
  }
}

static void item_update_tooltip(Item *item) {
  gchar *title = NULL, *desc = NULL;

  GVariant *tt = g_dbus_proxy_get_cached_property(item->proxy, "ToolTip");
  if (tt && g_variant_is_of_type(tt, G_VARIANT_TYPE("(sa(iiay)ss)")))
    g_variant_get(tt, "(&s@a(iiay)ss)", NULL, NULL, &title, &desc);
  if (tt)
    g_variant_unref(tt);

  if (!title || !*title) {
    g_free(title);
    title = prop_string(item->proxy, "Title");
  }

  GString *markup = g_string_new(NULL);
  if (title && *title) {
    gchar *t = g_markup_escape_text(title, -1);
    g_string_append_printf(markup, "<b>%s</b>", t);
    g_free(t);
  }
  if (desc && *desc) {
    // Descriptions may hold HTML; show them as plain text
    gchar *d = g_markup_escape_text(desc, -1);
    g_string_append_printf(markup, "%s%s", markup->len ? "\n" : "", d);
    g_free(d);
  }

  gtk_widget_set_tooltip_markup(item->button,
                                markup->len ? markup->str : NULL);
  g_string_free(markup, TRUE);
  g_free(title);
  g_free(desc);
}

static void item_update(Item *item) {
  if (!item->proxy)
    return;

  gchar *status = prop_string(item->proxy, "Status");
  GtkStyleContext *ctx = gtk_widget_get_style_context(item->button);
  gboolean passive = status && strcmp(status, "Passive") == 0;
  gboolean attention = status && strcmp(status, "NeedsAttention") == 0;
  g_free(status);

  if (passive)
    gtk_style_context_add_class(ctx, "passive");
  else
    gtk_style_context_remove_class(ctx, "passive");
  if (attention)
    gtk_style_context_add_class(ctx, "needs-attention");
  else
    gtk_style_context_remove_class(ctx, "needs-attention");

  item_update_icon(item);
  item_update_tooltip(item);
  gtk_widget_set_visible(item->button, !passive || item->tray->show_passive);
}

/* ------------------------------------------------------------------------ */
/* DBusMenu                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
  Item *item;
  gint32 id;
} MenuAction;

static void menu_item_activated(GtkMenuItem *mi, gpointer data) {
  (void)mi;
  MenuAction *a = data;
  Item *item = a->item;
  gchar *menu_path = prop_string(item->proxy, "Menu");
  if (!menu_path)
    return;
  g_dbus_connection_call(item->tray->bus, item->service, menu_path,
                         MENU_IFACE, "Event",
                         g_variant_new("(isvu)", a->id, "clicked",
                                       g_variant_new_int32(0),
                                       (guint32)(g_get_real_time() / 1000)),
                         NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  g_free(menu_path);
}

static GtkWidget *build_menu(Item *item, GVariant *children);

/* node: (ia{sv}av) */
static GtkWidget *build_menu_item(Item *item, GVariant *node) {
  if (!g_variant_is_of_type(node, G_VARIANT_TYPE("(ia{sv}av)")))
    return NULL;

  gint32 id;
  GVariant *props, *children;
  g_variant_get(node, "(i@a{sv}@av)", &id, &props, &children);

  GVariantDict d;
  g_variant_dict_init(&d, props);
  const char *type = NULL, *label = NULL, *toggle = NULL, *display = NULL;
  gboolean enabled = TRUE, visible = TRUE;
  gint32 state = 0;
  g_variant_dict_lookup(&d, "type", "&s", &type);
  g_variant_dict_lookup(&d, "label", "&s", &label);
  g_variant_dict_lookup(&d, "toggle-type", "&s", &toggle);
  g_variant_dict_lookup(&d, "children-display", "&s", &display);
  g_variant_dict_lookup(&d, "enabled", "b", &enabled);
  g_variant_dict_lookup(&d, "visible", "b", &visible);
  g_variant_dict_lookup(&d, "toggle-state", "i", &state);

  GtkWidget *mi = NULL;
  if (!visible) {
    mi = NULL;
  } else if (type && strcmp(type, "separator") == 0) {
    mi = gtk_separator_menu_item_new();
  } else {
    const char *text = label ? label : "";
    if (toggle && (strcmp(toggle, "checkmark") == 0 ||
                   strcmp(toggle, "radio") == 0)) {
      mi = gtk_check_menu_item_new_with_mnemonic(text);
      gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(mi),
                                            strcmp(toggle, "radio") == 0);
      // Set before connecting, set_active emits "activate"
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), state == 1);
    } else {
      mi = gtk_menu_item_new_with_mnemonic(text);
    }
    gtk_widget_set_sensitive(mi, enabled);

    if (g_variant_n_children(children) > 0 ||
        (display && strcmp(display, "submenu") == 0)) {
      gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), build_menu(item, children));
    } else {
      MenuAction *a = g_new0(MenuAction, 1);
      a->item = item;
      a->id = id;
      g_signal_connect_data(mi, "activate", G_CALLBACK(menu_item_activated),
                            a, (GClosureNotify)(void (*)(void))g_free, 0);
    }
  }

  g_variant_dict_clear(&d);
  g_variant_unref(props);
  g_variant_unref(children);
  return mi;
}

/* children: av of (ia{sv}av) */
static GtkWidget *build_menu(Item *item, GVariant *children) {
  GtkWidget *menu = gtk_menu_new();
  GVariantIter iter;
  GVariant *child;
  g_variant_iter_init(&iter, children);
  while ((child = g_variant_iter_next_value(&iter))) {
    GVariant *node = g_variant_get_variant(child);
    GtkWidget *mi = build_menu_item(item, node);
    if (mi)
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_variant_unref(node);
    g_variant_unref(child);
  }
  gtk_widget_show_all(menu);
  return menu;
}

typedef struct {
  Item *item;
  GdkEvent *event;
} MenuRequest;

static void layout_ready(GObject *src, GAsyncResult *res, gpointer data) {
  MenuRequest *req = data;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);

  if (reply && !g_cancellable_is_cancelled(req->item->cancel)) {
    Item *item = req->item;
    GVariant *layout = NULL;
    g_variant_get(reply, "(u@(ia{sv}av))", NULL, &layout);

    GVariant *children = g_variant_get_child_value(layout, 2);
    if (item->menu)
      gtk_widget_destroy(item->menu);
    item->menu = build_menu(item, children);
    gtk_menu_attach_to_widget(GTK_MENU(item->menu), item->button, NULL);
    gtk_menu_popup_at_widget(GTK_MENU(item->menu), item->button,
                             GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH,
                             req->event);
    g_variant_unref(children);
    g_variant_unref(layout);
  }

  if (reply)
    g_variant_unref(reply);
  gdk_event_free(req->event);
  g_free(req);
}

static gboolean item_show_menu(Item *item, GdkEvent *event) {
  gchar *menu_path = prop_string(item->proxy, "Menu");
  if (!menu_path || strcmp(menu_path, "/") == 0) {
    g_free(menu_path);
    return FALSE;
  }

  g_dbus_connection_call(item->tray->bus, item->service, menu_path,
                         MENU_IFACE, "AboutToShow", g_variant_new("(i)", 0),
                         NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

  MenuRequest *req = g_new0(MenuRequest, 1);
  req->item = item;
  req->event = gdk_event_copy(event);
  const gchar *no_props[] = {NULL};
  g_dbus_connection_call(item->tray->bus, item->service, menu_path,
                         MENU_IFACE, "GetLayout",
                         g_variant_new("(ii^as)", 0, -1, no_props),
                         G_VARIANT_TYPE("(u(ia{sv}av))"),
                         G_DBUS_CALL_FLAGS_NONE, -1, item->cancel,
                         layout_ready, req);
  g_free(menu_path);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  Item *item;
  GdkEvent *event;
} ActivateRequest;

/* Activate failed (e.g. not implemented): fall back to the menu */
static void activate_done(GObject *src, GAsyncResult *res, gpointer data) {
  ActivateRequest *req = data;
  GError *err = NULL;
  GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
  if (reply)
    g_variant_unref(reply);
  // On cancellation the item is already freed
  if (err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    item_show_menu(req->item, req->event);
  if (err)
    g_error_free(err);
  gdk_event_free(req->event);
  g_free(req);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  (void)w;
  Item *item = data;
  if (ev->type != GDK_BUTTON_PRESS || !item->proxy)
    return FALSE;

  gint x = (gint)ev->x_root, y = (gint)ev->y_root;

  if (ev->button == 1) {
    if (prop_bool(item->proxy, "ItemIsMenu") &&
        item_show_menu(item, (GdkEvent *)ev))
      return TRUE;
    ActivateRequest *req = g_new0(ActivateRequest, 1);
    req->item = item;
    req->event = gdk_event_copy((GdkEvent *)ev);
    g_dbus_proxy_call(item->proxy, "Activate", g_variant_new("(ii)", x, y),
                      G_DBUS_CALL_FLAGS_NONE, -1, item->cancel, activate_done,
                      req);
  } else if (ev->button == 2) {
    g_dbus_proxy_call(item->proxy, "SecondaryActivate",
                      g_variant_new("(ii)", x, y), G_DBUS_CALL_FLAGS_NONE, -1,
                      NULL, NULL, NULL);
  } else if (ev->button == 3) {
    if (!item_show_menu(item, (GdkEvent *)ev))
      g_dbus_proxy_call(item->proxy, "ContextMenu",
                        g_variant_new("(ii)", x, y), G_DBUS_CALL_FLAGS_NONE,
                        -1, NULL, NULL, NULL);
  }
  return TRUE;
}

static void send_scroll(Item *item, int delta, const char *orientation) {
  g_dbus_proxy_call(item->proxy, "Scroll",
                    g_variant_new("(is)", delta, orientation),
                    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data) {
  (void)w;
  Item *item = data;
  if (!item->proxy)
    return FALSE;

  switch (ev->direction) {
  case GDK_SCROLL_UP:
    send_scroll(item, -1, "vertical");
    break;
  case GDK_SCROLL_DOWN:
    send_scroll(item, 1, "vertical");
    break;
  case GDK_SCROLL_LEFT:
    send_scroll(item, -1, "horizontal");
    break;
  case GDK_SCROLL_RIGHT:
    send_scroll(item, 1, "horizontal");
    break;
  case GDK_SCROLL_SMOOTH:
    // Touchpads: send whole steps only
    item->scroll_dx += ev->delta_x;
    item->scroll_dy += ev->delta_y;
    if ((int)item->scroll_dy != 0) {
      send_scroll(item, (int)item->scroll_dy, "vertical");
      item->scroll_dy -= (int)item->scroll_dy;
    }
    if ((int)item->scroll_dx != 0) {
      send_scroll(item, (int)item->scroll_dx, "horizontal");
      item->scroll_dx -= (int)item->scroll_dx;
    }
    break;
  }
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Items                                                                    */
/* ------------------------------------------------------------------------ */

static void item_free(Item *item) {
  g_cancellable_cancel(item->cancel);
  g_object_unref(item->cancel);
  if (item->proxy) {
    g_signal_handlers_disconnect_by_data(item->proxy, item);
    g_object_unref(item->proxy);
  }
  if (item->menu)
    gtk_widget_destroy(item->menu);
  if (item->button)
    gtk_widget_destroy(item->button);
  g_free(item->id);
  g_free(item->service);
  g_free(item->path);
  g_free(item);
}

static void props_ready(GObject *src, GAsyncResult *res, gpointer data) {
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
  if (!reply)
    return;

  Item *item = data; // only valid if not cancelled, checked by GIO
  GVariant *dict = g_variant_get_child_value(reply, 0);
  GVariantIter iter;
  const gchar *key;
  GVariant *value;
  g_variant_iter_init(&iter, dict);
  while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
    g_dbus_proxy_set_cached_property(item->proxy, key, value);
    g_variant_unref(value);
  }
  g_variant_unref(dict);
  g_variant_unref(reply);
  item_update(item);
}

/* Items announce changes with New* signals instead of PropertiesChanged,
 * so fetch everything again */
static void item_refresh(Item *item) {
  g_dbus_connection_call(item->tray->bus, item->service, item->path,
                         "org.freedesktop.DBus.Properties", "GetAll",
                         g_variant_new("(s)", ITEM_IFACE),
                         G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
                         -1, item->cancel, props_ready, item);
}

static void on_item_signal(GDBusProxy *proxy, gchar *sender, gchar *signal,
                           GVariant *params, gpointer data) {
  (void)proxy;
  (void)sender;
  (void)params;
  if (g_str_has_prefix(signal, "New"))
    item_refresh(data);
}

static void on_owner_changed(GObject *obj, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  Item *item = data;
  gchar *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(obj));
  if (!owner)
    tray_remove_item(item->tray, item->id);
  g_free(owner);
}

static void proxy_ready(GObject *src, GAsyncResult *res, gpointer data) {
  (void)src;
  GError *err = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_finish(res, &err);
  if (!proxy) {
    // Cancelled means the item is already gone
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      Item *item = data;
      tray_remove_item(item->tray, item->id);
    }
    g_error_free(err);
    return;
  }

  Item *item = data;
  item->proxy = proxy;
  g_signal_connect(proxy, "g-signal", G_CALLBACK(on_item_signal), item);
  g_signal_connect(proxy, "notify::g-name-owner", G_CALLBACK(on_owner_changed),
                   item);
  item_update(item);
}

static Item *find_item(Tray *tray, const char *id) {
  for (GList *l = tray->items; l; l = l->next)
    if (strcmp(((Item *)l->data)->id, id) == 0)
      return l->data;
  return NULL;
}

static void tray_add_item(Tray *tray, const char *id) {
  if (!id || !*id || find_item(tray, id))
    return;

  Item *item = g_new0(Item, 1);
  item->tray = tray;
  item->id = g_strdup(id);
  const char *slash = strchr(id, '/');
  item->service = slash ? g_strndup(id, slash - id) : g_strdup(id);
  item->path = g_strdup(slash ? slash : "/StatusNotifierItem");
  item->cancel = g_cancellable_new();

  item->button = gtk_event_box_new();
  item->image = gtk_image_new();
  gtk_container_add(GTK_CONTAINER(item->button), item->image);
  gtk_style_context_add_class(gtk_widget_get_style_context(item->button),
                              "item");
  gtk_widget_add_events(item->button,
                        GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK |
                            GDK_SMOOTH_SCROLL_MASK);
  g_signal_connect(item->button, "button-press-event", G_CALLBACK(on_button),
                   item);
  g_signal_connect(item->button, "scroll-event", G_CALLBACK(on_scroll), item);
  gtk_box_pack_start(GTK_BOX(tray->box), item->button, FALSE, FALSE, 0);
  gtk_widget_show(item->image);

  tray->items = g_list_append(tray->items, item);

  g_dbus_proxy_new(tray->bus, G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                   NULL, item->service, item->path, ITEM_IFACE, item->cancel,
                   proxy_ready, item);
}

static void tray_remove_item(Tray *tray, const char *id) {
  Item *item = find_item(tray, id);
  if (!item)
    return;
  tray->items = g_list_remove(tray->items, item);
  item_free(item);
}

static void tray_clear_items(Tray *tray) {
  while (tray->items) {
    Item *item = tray->items->data;
    tray->items = g_list_remove(tray->items, item);
    item_free(item);
  }
}

static void on_style_updated(GtkWidget *w, gpointer data) {
  (void)w;
  Tray *tray = data;
  if (tray->updating)
    return;
  // Theme or colours changed: recolour the symbolic icons
  tray->updating = TRUE;
  for (GList *l = tray->items; l; l = l->next)
    item_update(l->data);
  tray->updating = FALSE;
}

/* ------------------------------------------------------------------------ */
/* StatusNotifierWatcher (when nobody else provides one)                    */
/* ------------------------------------------------------------------------ */

static const char watcher_xml[] =
    "<node>"
    " <interface name='org.kde.StatusNotifierWatcher'>"
    "  <method name='RegisterStatusNotifierItem'>"
    "   <arg name='service' type='s' direction='in'/>"
    "  </method>"
    "  <method name='RegisterStatusNotifierHost'>"
    "   <arg name='service' type='s' direction='in'/>"
    "  </method>"
    "  <property name='RegisteredStatusNotifierItems' type='as' "
    "access='read'/>"
    "  <property name='IsStatusNotifierHostRegistered' type='b' "
    "access='read'/>"
    "  <property name='ProtocolVersion' type='i' access='read'/>"
    "  <signal name='StatusNotifierItemRegistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierItemUnregistered'><arg type='s'/></signal>"
    "  <signal name='StatusNotifierHostRegistered'/>"
    "  <signal name='StatusNotifierHostUnregistered'/>"
    " </interface>"
    "</node>";

static void watched_free(gpointer data) {
  WatchedItem *w = data;
  if (w->name_watch)
    g_bus_unwatch_name(w->name_watch);
  g_free(w->id);
  g_free(w);
}

static void emit_watcher_signal(Tray *tray, const char *name, const char *id) {
  g_dbus_connection_emit_signal(tray->bus, NULL, WATCHER_PATH, WATCHER_NAME,
                                name, id ? g_variant_new("(s)", id) : NULL,
                                NULL);
}

static void on_item_vanished(GDBusConnection *c, const gchar *name,
                             gpointer data) {
  (void)c;
  (void)name;
  WatchedItem *w = data;
  Tray *tray = w->tray;
  gchar *id = g_strdup(w->id);
  g_ptr_array_remove(tray->watched, w); // frees w
  emit_watcher_signal(tray, "StatusNotifierItemUnregistered", id);
  g_free(id);
}

static void watcher_register_item(Tray *tray, const char *sender,
                                  const char *service) {
  // Either an object path on the sender (Ayatana) or a bus name
  gchar *id = (service[0] == '/') ? g_strconcat(sender, service, NULL)
                                  : g_strconcat(service, "/StatusNotifierItem",
                                                NULL);
  for (guint i = 0; i < tray->watched->len; i++) {
    if (strcmp(((WatchedItem *)tray->watched->pdata[i])->id, id) == 0) {
      g_free(id);
      return;
    }
  }

  WatchedItem *w = g_new0(WatchedItem, 1);
  w->tray = tray;
  w->id = id;
  gchar *bus_name = (service[0] == '/') ? g_strdup(sender) : g_strdup(service);
  w->name_watch = g_bus_watch_name_on_connection(
      tray->bus, bus_name, G_BUS_NAME_WATCHER_FLAGS_NONE, NULL,
      on_item_vanished, w, NULL);
  g_free(bus_name);
  g_ptr_array_add(tray->watched, w);

  emit_watcher_signal(tray, "StatusNotifierItemRegistered", id);
}

static void watcher_method(GDBusConnection *c, const gchar *sender,
                           const gchar *path, const gchar *iface,
                           const gchar *method, GVariant *params,
                           GDBusMethodInvocation *inv, gpointer data) {
  (void)c;
  (void)path;
  (void)iface;
  Tray *tray = data;
  const gchar *service;
  g_variant_get(params, "(&s)", &service);

  if (strcmp(method, "RegisterStatusNotifierItem") == 0)
    watcher_register_item(tray, sender, service);
  else if (strcmp(method, "RegisterStatusNotifierHost") == 0)
    emit_watcher_signal(tray, "StatusNotifierHostRegistered", NULL);

  g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *watcher_get(GDBusConnection *c, const gchar *sender,
                             const gchar *path, const gchar *iface,
                             const gchar *prop, GError **err, gpointer data) {
  (void)c;
  (void)sender;
  (void)path;
  (void)iface;
  (void)err;
  Tray *tray = data;

  if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    for (guint i = 0; i < tray->watched->len; i++)
      g_variant_builder_add(&b, "s",
                            ((WatchedItem *)tray->watched->pdata[i])->id);
    return g_variant_builder_end(&b);
  }
  if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0)
    return g_variant_new_boolean(TRUE);
  if (strcmp(prop, "ProtocolVersion") == 0)
    return g_variant_new_int32(0);
  return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {watcher_method,
                                                    watcher_get, NULL, {0}};

static void on_watcher_acquired(GDBusConnection *c, const gchar *name,
                                gpointer data) {
  (void)name;
  Tray *tray = data;
  GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
  if (!info)
    return;
  tray->watcher_reg = g_dbus_connection_register_object(
      c, WATCHER_PATH, info->interfaces[0], &watcher_vtable, tray, NULL, NULL);
  g_dbus_node_info_unref(info);
}

static void on_watcher_lost(GDBusConnection *c, const gchar *name,
                            gpointer data) {
  (void)name;
  Tray *tray = data;
  // Another watcher (e.g. Waybar's own tray) runs; we only act as a host
  if (tray->watcher_reg && c) {
    g_dbus_connection_unregister_object(c, tray->watcher_reg);
    tray->watcher_reg = 0;
  }
  g_ptr_array_set_size(tray->watched, 0);
}

/* ------------------------------------------------------------------------ */
/* StatusNotifierHost                                                       */
/* ------------------------------------------------------------------------ */

static void on_watcher_signal(GDBusConnection *c, const gchar *sender,
                              const gchar *path, const gchar *iface,
                              const gchar *signal, GVariant *params,
                              gpointer data) {
  (void)c;
  (void)sender;
  (void)path;
  (void)iface;
  const gchar *id;
  if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(s)")))
    return;
  g_variant_get(params, "(&s)", &id);
  if (strcmp(signal, "StatusNotifierItemRegistered") == 0)
    tray_add_item(data, id);
  else
    tray_remove_item(data, id);
}

static void items_ready(GObject *src, GAsyncResult *res, gpointer data) {
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
  if (!reply)
    return;
  GVariant *v = NULL;
  g_variant_get(reply, "(v)", &v);
  if (v && g_variant_is_of_type(v, G_VARIANT_TYPE("as"))) {
    GVariantIter iter;
    const gchar *id;
    g_variant_iter_init(&iter, v);
    while (g_variant_iter_next(&iter, "&s", &id))
      tray_add_item(data, id);
  }
  if (v)
    g_variant_unref(v);
  g_variant_unref(reply);
}

static void on_watcher_appeared(GDBusConnection *c, const gchar *name,
                                const gchar *owner, gpointer data) {
  (void)name;
  (void)owner;
  Tray *tray = data;
  g_dbus_connection_call(c, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
                         "RegisterStatusNotifierHost",
                         g_variant_new("(s)", tray->host_name), NULL,
                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  g_dbus_connection_call(
      c, WATCHER_NAME, WATCHER_PATH, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", WATCHER_NAME, "RegisteredStatusNotifierItems"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, tray->cancel,
      items_ready, tray);
}

static void on_watcher_vanished(GDBusConnection *c, const gchar *name,
                                gpointer data) {
  (void)c;
  (void)name;
  // Items register again with the next watcher
  tray_clear_items(data);
}

/* ------------------------------------------------------------------------ */
/* CFFI entry points                                                        */
/* ------------------------------------------------------------------------ */

void *wbcffi_init(const wbcffi_init_info *info,
                  const wbcffi_config_entry *entries, size_t len) {
  Tray *tray = g_new0(Tray, 1);
  tray->obj = info->obj;
  tray->icon_size = 16;
  tray->spacing = 8;
  tray->prefer_symbolic = TRUE;
  parse_config(tray, entries, len);

  tray->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!tray->bus) {
    g_free(tray);
    return NULL;
  }
  tray->cancel = g_cancellable_new();

  GtkContainer *root = info->get_root_widget(info->obj);
  tray->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, tray->spacing);
  gtk_style_context_add_class(gtk_widget_get_style_context(tray->box),
                              "tray");
  g_signal_connect(tray->box, "style-updated", G_CALLBACK(on_style_updated),
                   tray);
  gtk_container_add(root, tray->box);
  gtk_widget_show(tray->box);

  tray->watched = g_ptr_array_new_with_free_func(watched_free);
  tray->watcher_own = g_bus_own_name_on_connection(
      tray->bus, WATCHER_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
      on_watcher_acquired, on_watcher_lost, tray, NULL);

  // Several bars load the module, so the host name has to be unique
  static int instance = 0;
  tray->host_name = g_strdup_printf("org.kde.StatusNotifierHost-%d-%d",
                                    (int)getpid(), ++instance);
  tray->host_own = g_bus_own_name_on_connection(
      tray->bus, tray->host_name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL,
      NULL, NULL);

  tray->sub_registered = g_dbus_connection_signal_subscribe(
      tray->bus, NULL, WATCHER_NAME, "StatusNotifierItemRegistered",
      WATCHER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_watcher_signal, tray,
      NULL);
  tray->sub_unregistered = g_dbus_connection_signal_subscribe(
      tray->bus, NULL, WATCHER_NAME, "StatusNotifierItemUnregistered",
      WATCHER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_watcher_signal, tray,
      NULL);
  tray->watcher_watch = g_bus_watch_name_on_connection(
      tray->bus, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
      on_watcher_appeared, on_watcher_vanished, tray, NULL);

  return tray;
}

void wbcffi_deinit(void *instance) {
  Tray *tray = instance;
  if (!tray)
    return;

  g_cancellable_cancel(tray->cancel);
  g_object_unref(tray->cancel);
  g_bus_unwatch_name(tray->watcher_watch);
  g_dbus_connection_signal_unsubscribe(tray->bus, tray->sub_registered);
  g_dbus_connection_signal_unsubscribe(tray->bus, tray->sub_unregistered);
  g_bus_unown_name(tray->host_own);
  if (tray->watcher_reg)
    g_dbus_connection_unregister_object(tray->bus, tray->watcher_reg);
  g_bus_unown_name(tray->watcher_own);
  g_ptr_array_free(tray->watched, TRUE);

  tray_clear_items(tray);
  g_free(tray->host_name);
  g_object_unref(tray->bus);
  g_free(tray);
}
