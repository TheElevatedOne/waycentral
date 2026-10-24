#define _GNU_SOURCE
#include "../../waycentral.h"
#include "../util.h"
#include "updates.h"
#include <alpm.h>
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define PACMAN_CONF "/etc/pacman.conf"
#define MAX_SERVERS 64

struct repo_conf {
  char name[64];
  int server_count;
  char *servers[MAX_SERVERS];
};

struct pacman_conf {
  char root[256];
  char dbpath[256];
  char arch[64];
  int repo_count;
  struct repo_conf repos[WC_MAX_REPOS];
};

static char *trim(char *s) {
  while (isspace((unsigned char)*s))
    s++;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1]))
    *--end = '\0';
  return s;
}

/* Replaces $repo and $arch in a Server= url */
static char *expand_server(const char *url, const char *repo,
                           const char *arch) {
  struct wc_buf buf = {0};
  for (const char *c = url; *c;) {
    if (strncmp(c, "$repo", 5) == 0) {
      wc_buf_appendf(&buf, "%s", repo);
      c += 5;
    } else if (strncmp(c, "$arch", 5) == 0) {
      wc_buf_appendf(&buf, "%s", arch);
      c += 5;
    } else {
      wc_buf_appendf(&buf, "%c", *c++);
    }
  }
  return wc_buf_finish(&buf);
}

static void add_server(struct pacman_conf *conf, struct repo_conf *repo,
                       const char *url) {
  if (repo->server_count >= MAX_SERVERS)
    return;
  char *server = expand_server(url, repo->name, conf->arch);
  if (server)
    repo->servers[repo->server_count++] = server;
}

static void parse_conf_file(struct pacman_conf *conf, const char *path,
                            struct repo_conf *repo, int depth);

static void include_files(struct pacman_conf *conf, const char *pattern,
                          struct repo_conf *repo, int depth) {
  glob_t g;
  if (depth > 8 || glob(pattern, GLOB_NOCHECK, NULL, &g) != 0)
    return;
  for (size_t i = 0; i < g.gl_pathc; i++)
    parse_conf_file(conf, g.gl_pathv[i], repo, depth + 1);
  globfree(&g);
}

/* repo is the section the file is included from (NULL for [options]) */
static void parse_conf_file(struct pacman_conf *conf, const char *path,
                            struct repo_conf *repo, int depth) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;

  char raw[1024];
  int in_options = 0;
  while (fgets(raw, sizeof(raw), f)) {
    char *hash = strchr(raw, '#');
    if (hash)
      *hash = '\0';
    char *line = trim(raw);
    if (!*line)
      continue;

    if (line[0] == '[') {
      char *end = strchr(line, ']');
      if (!end)
        continue;
      *end = '\0';
      in_options = strcmp(line + 1, "options") == 0;
      repo = NULL;
      if (!in_options && conf->repo_count < WC_MAX_REPOS) {
        repo = &conf->repos[conf->repo_count++];
        memset(repo, 0, sizeof(*repo));
        snprintf(repo->name, sizeof(repo->name), "%s", line + 1);
      }
      continue;
    }

    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = trim(line);
    char *value = trim(eq + 1);

    if (in_options) {
      if (strcmp(key, "RootDir") == 0)
        snprintf(conf->root, sizeof(conf->root), "%s", value);
      else if (strcmp(key, "DBPath") == 0)
        snprintf(conf->dbpath, sizeof(conf->dbpath), "%s", value);
      else if (strcmp(key, "Include") == 0)
        include_files(conf, value, NULL, depth);
    } else if (repo) {
      if (strcmp(key, "Server") == 0)
        add_server(conf, repo, value);
      else if (strcmp(key, "Include") == 0)
        include_files(conf, value, repo, depth);
    }
  }

  fclose(f);
}

static void free_conf(struct pacman_conf *conf) {
  for (int i = 0; i < conf->repo_count; i++)
    for (int j = 0; j < conf->repos[i].server_count; j++)
      free(conf->repos[i].servers[j]);
}

static void load_conf(struct pacman_conf *conf) {
  memset(conf, 0, sizeof(*conf));
  snprintf(conf->root, sizeof(conf->root), "/");
  snprintf(conf->dbpath, sizeof(conf->dbpath), "/var/lib/pacman/");

  /* Architecture has to be known before Server lines are expanded, so read
   * [options] first */
  FILE *f = fopen(PACMAN_CONF, "r");
  if (f) {
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      char *l = trim(line);
      if (strncmp(l, "Architecture", 12) == 0 && strchr(l, '='))
        sscanf(trim(strchr(l, '=') + 1), "%63s", conf->arch);
    }
    fclose(f);
  }
  if (!conf->arch[0] || strcmp(conf->arch, "auto") == 0) {
    struct utsname u;
    uname(&u);
    snprintf(conf->arch, sizeof(conf->arch), "%s", u.machine);
  }

  parse_conf_file(conf, PACMAN_CONF, NULL, 0);
}

static int mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  return (mkdir(tmp, 0700) != 0 && errno != EEXIST) ? -1 : 0;
}

/* Private dbpath: sync/ is ours, local/ links to the real local db */
static int prepare_dbpath(const struct pacman_conf *conf, char *out,
                          size_t size) {
  const char *cache = getenv("XDG_CACHE_HOME");
  const char *home = getenv("HOME");
  if (cache && *cache)
    snprintf(out, size, "%s/waycentral/pacman", cache);
  else if (home && *home)
    snprintf(out, size, "%s/.cache/waycentral/pacman", home);
  else
    snprintf(out, size, "/tmp/waycentral-%d/pacman", (int)getuid());

  char path[600];
  snprintf(path, sizeof(path), "%s/sync", out);
  if (mkdir_p(path) != 0)
    return -1;

  char local[600], target[512];
  snprintf(local, sizeof(local), "%s/local", out);
  snprintf(target, sizeof(target), "%s/local", conf->dbpath);

  char current[512];
  ssize_t n = readlink(local, current, sizeof(current) - 1);
  if (n >= 0) {
    current[n] = '\0';
    if (strcmp(current, target) == 0)
      return 0;
  }
  unlink(local);
  return symlink(target, local);
}

static int find_repo(const struct wc_updates *out, const char *name) {
  for (int i = 0; i < out->repo_count; i++)
    if (strcmp(out->repos[i].name, name) == 0)
      return i;
  return -1;
}

int wc_updates_check(struct wc_updates *out, int sync, int aur) {
  memset(out, 0, sizeof(*out));
  out->aur = -1;
  out->checked = (long)time(NULL);

  struct pacman_conf conf;
  load_conf(&conf);

  char dbpath[512];
  if (prepare_dbpath(&conf, dbpath, sizeof(dbpath)) != 0) {
    snprintf(out->error, sizeof(out->error), "Cannot create %s", dbpath);
    free_conf(&conf);
    return -1;
  }

  alpm_errno_t err;
  alpm_handle_t *handle = alpm_initialize(conf.root, dbpath, &err);
  if (!handle) {
    snprintf(out->error, sizeof(out->error), "libalpm: %s",
             alpm_strerror(err));
    free_conf(&conf);
    return -1;
  }

  for (int i = 0; i < conf.repo_count; i++) {
    struct repo_conf *repo = &conf.repos[i];
    // Only reading versions, so signatures are not checked
    alpm_db_t *db = alpm_register_syncdb(handle, repo->name, 0);
    if (!db)
      continue;
    for (int j = 0; j < repo->server_count; j++)
      alpm_db_add_server(db, repo->servers[j]);

    snprintf(out->repos[out->repo_count].name,
             sizeof(out->repos[0].name), "%s", repo->name);
    out->repo_count++;
  }
  free_conf(&conf);

  alpm_list_t *syncdbs = alpm_get_syncdbs(handle);

  if (sync && alpm_db_update(handle, syncdbs, 0) < 0)
    snprintf(out->error, sizeof(out->error),
             "Sync failed (%s), using cached databases",
             alpm_strerror(alpm_errno(handle)));

  struct wc_foreign_pkg *foreign = NULL;
  size_t foreign_cap = 0;

  alpm_list_t *local = alpm_db_get_pkgcache(alpm_get_localdb(handle));
  for (alpm_list_t *i = local; i; i = alpm_list_next(i)) {
    alpm_pkg_t *pkg = i->data;
    const char *name = alpm_pkg_get_name(pkg);

    int in_repo = 0;
    for (alpm_list_t *j = syncdbs; j && !in_repo; j = alpm_list_next(j))
      in_repo = alpm_db_get_pkg(j->data, name) != NULL;

    if (!in_repo) {
      if ((size_t)out->foreign == foreign_cap) {
        foreign_cap = foreign_cap ? foreign_cap * 2 : 64;
        struct wc_foreign_pkg *grown =
            realloc(foreign, foreign_cap * sizeof(*foreign));
        if (!grown)
          break;
        foreign = grown;
      }
      foreign[out->foreign].name = name;
      foreign[out->foreign].version = alpm_pkg_get_version(pkg);
      out->foreign++;
      continue;
    }

    alpm_pkg_t *newer = alpm_sync_get_new_version(pkg, syncdbs);
    if (!newer)
      continue;
    int idx = find_repo(out, alpm_db_get_name(alpm_pkg_get_db(newer)));
    if (idx >= 0)
      out->repos[idx].count++;
    out->total++;
  }

  if (aur) {
    char aur_err[128] = "";
    out->aur =
        wc_aur_count_updates(foreign, (size_t)out->foreign, aur_err,
                             sizeof(aur_err));
    if (out->aur >= 0)
      out->total += out->aur;
    else if (!out->error[0])
      snprintf(out->error, sizeof(out->error), "%s", aur_err);
  }

  free(foreign);
  alpm_release(handle);
  return 0;
}

char *wc_updates_json(const struct wc_updates *u) {
  struct wc_buf json = {0};
  const char *state = u->error[0] ? "error" : (u->total ? "updates" : "none");

  wc_buf_appendf(&json, "{\"text\":\"%d\",\"alt\":\"%s\",\"class\":\"%s\","
                        "\"tooltip\":\"<b>Updates:</b>\\t%d\\n",
                 u->total, state, state, u->total);

  for (int i = 0; i < u->repo_count; i++) {
    wc_buf_append_text(&json, u->repos[i].name);
    wc_buf_appendf(&json, ":\\t%d\\n", u->repos[i].count);
  }
  if (u->aur >= 0)
    wc_buf_appendf(&json, "aur:\\t%d\\n", u->aur);

  char when[32] = "never";
  if (u->checked) {
    time_t t = (time_t)u->checked;
    struct tm local;
    localtime_r(&t, &local);
    strftime(when, sizeof(when), "%H:%M", &local);
  }
  wc_buf_appendf(&json, "\\nChecked:\\t%s", when);

  if (u->error[0]) {
    wc_buf_appendf(&json, "\\n\\n");
    wc_buf_append_text(&json, u->error);
  }

  wc_buf_appendf(&json, "\"}");
  return wc_buf_finish(&json);
}

static int emit_check(int sync, int aur, wc_json_cb cb, void *userdata) {
  struct wc_updates u;
  wc_updates_check(&u, sync, aur);
  char *json = wc_updates_json(&u);
  int stop = json ? cb(json, userdata) : 0;
  free(json);
  return stop;
}

static long long now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

int wc_updates_watch(int interval_min, int aur, wc_json_cb cb,
                     void *userdata) {
  if (interval_min <= 0)
    interval_min = 30;

  struct pacman_conf conf;
  load_conf(&conf);
  free_conf(&conf);

  /* pacman removes db.lck when a transaction is done; recount then */
  int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (ino >= 0)
    inotify_add_watch(ino, conf.dbpath, IN_DELETE);

  int ret = 0;
  if (emit_check(1, aur, cb, userdata))
    goto out;

  long long next = now_s() + interval_min * 60LL;
  for (;;) {
    long long left = next - now_s();
    struct pollfd pfd = {.fd = ino, .events = POLLIN};
    int r = poll(&pfd, ino >= 0 ? 1 : 0,
                 left > 0 ? (int)(left * 1000) : 0);

    int sync = 0;
    if (r < 0 && errno == EINTR) {
      sync = 1; // forced check
    } else if (r < 0) {
      ret = -1;
      break;
    } else if (r == 0) {
      sync = 1; // interval passed
    } else {
      char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
      int lock_removed = 0;
      ssize_t n;
      while ((n = read(ino, buf, sizeof(buf))) > 0) {
        for (char *p = buf; p < buf + n;) {
          struct inotify_event *ev = (struct inotify_event *)p;
          if (ev->len && strcmp(ev->name, "db.lck") == 0)
            lock_removed = 1;
          p += sizeof(*ev) + ev->len;
        }
      }
      if (!lock_removed)
        continue;
    }

    if (emit_check(sync, aur, cb, userdata))
      break;
    if (sync)
      next = now_s() + interval_min * 60LL;
  }

out:
  if (ino >= 0)
    close(ino);
  return ret;
}
