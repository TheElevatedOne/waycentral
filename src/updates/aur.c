#include "../../waycentral.h"
#include "../util.h"
#include "updates.h"
#include <alpm.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#define AUR_RPC "https://aur.archlinux.org/rpc/v5/info"
#define AUR_BATCH 150

static size_t write_cb(char *data, size_t size, size_t nmemb, void *userdata) {
  struct wc_buf *buf = userdata;
  size_t n = size * nmemb;
  wc_buf_appendf(buf, "%.*s", (int)n, data);
  return buf->failed ? 0 : n;
}

/* Finds the installed version of name, pkgs is short enough to scan */
static const char *local_version(const struct wc_foreign_pkg *pkgs,
                                 size_t count, const char *name) {
  for (size_t i = 0; i < count; i++)
    if (strcmp(pkgs[i].name, name) == 0)
      return pkgs[i].version;
  return NULL;
}

/* Queries one batch, returns the number of updates in it or -1 */
static int query_batch(CURL *curl, const struct wc_foreign_pkg *pkgs,
                       size_t count, char *err, size_t err_size) {
  struct wc_buf body = {0};
  for (size_t i = 0; i < count; i++) {
    char *name = curl_easy_escape(curl, pkgs[i].name, 0);
    if (!name)
      continue;
    wc_buf_appendf(&body, "%sarg[]=%s", i ? "&" : "", name);
    curl_free(name);
  }
  char *post = wc_buf_finish(&body);
  if (!post) {
    snprintf(err, err_size, "Out of memory");
    return -1;
  }

  struct wc_buf response = {0};
  curl_easy_setopt(curl, CURLOPT_URL, AUR_RPC);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  CURLcode res = curl_easy_perform(curl);
  free(post);
  char *json = wc_buf_finish(&response);

  if (res != CURLE_OK || !json) {
    snprintf(err, err_size, "AUR: %s", curl_easy_strerror(res));
    free(json);
    return -1;
  }

  json_object *root = json_tokener_parse(json);
  free(json);

  json_object *results;
  if (!root || !json_object_object_get_ex(root, "results", &results) ||
      !json_object_is_type(results, json_type_array)) {
    snprintf(err, err_size, "AUR: Invalid response");
    json_object_put(root);
    return -1;
  }

  int updates = 0;
  size_t n = json_object_array_length(results);
  for (size_t i = 0; i < n; i++) {
    json_object *pkg = json_object_array_get_idx(results, i);
    json_object *name, *version;
    if (!json_object_object_get_ex(pkg, "Name", &name) ||
        !json_object_object_get_ex(pkg, "Version", &version))
      continue;

    const char *local =
        local_version(pkgs, count, json_object_get_string(name));
    if (local && alpm_pkg_vercmp(json_object_get_string(version), local) > 0)
      updates++;
  }

  json_object_put(root);
  return updates;
}

int wc_aur_count_updates(const struct wc_foreign_pkg *pkgs, size_t count,
                         char *err, size_t err_size) {
  if (count == 0)
    return 0;

  static int curl_ready = 0;
  if (!curl_ready) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
      snprintf(err, err_size, "AUR: curl init failed");
      return -1;
    }
    curl_ready = 1;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    snprintf(err, err_size, "AUR: curl init failed");
    return -1;
  }

  char agent[64];
  snprintf(agent, sizeof(agent), "waycentral/%s", wc_version());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, agent);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  int total = 0;
  for (size_t i = 0; i < count; i += AUR_BATCH) {
    size_t n = (count - i < AUR_BATCH) ? count - i : AUR_BATCH;
    int r = query_batch(curl, pkgs + i, n, err, err_size);
    if (r < 0) {
      total = -1;
      break;
    }
    total += r;
  }

  curl_easy_cleanup(curl);
  return total;
}
