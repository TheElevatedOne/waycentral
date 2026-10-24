#define _GNU_SOURCE
#include "waycentral.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MUSIC_LENGTH 40
#define DATE_RESET_S 120

static void help_message(void) {
  printf(
      "> waycentral [-h/--help] [OPTIONS]\n\n"
      "Centralized Program for Waybar Modules\n\n"
      "HELP:\n"
      "  -h, --help    Show this help message\n"
      "  -v, --version Show version\n"
      "  Defaults are MY preferences (shown with a *).\n\n"
      "OPTIONS:\n"
      "  -c, --clock [24H, 12AP, 12H*]\n"
      "                Print Waybar JSON for clock.\n"
      "                24H -> 24 Hour clock;\n"
      "                12AP -> 12 Hour clock with AM/PM;\n"
      "                12H -> 12 Hour clock without AM/PM.\n"
      "  -d, --date [DMY, MDY, YMD*] [M*, S]\n"
      "                Print Waybar JSON for date.\n"
      "                DMY -> Day/Month/Year;\n"
      "                MDY -> Month/Day/Year;\n"
      "                YMD -> Year/Month/Day.\n"
      "                M -> Monday First in Calendar Tooltip;\n"
      "                S -> Sunday First in Calendar Tooltip.\n"
      "  -d, --date [next, prev, today]\n"
      "                Change the month of the calendar tooltip\n"
      "                of the running date modules.\n"
      "  -b, --battery Print Waybar JSON for battery.\n"
      "  -s, --sysmon [CPU, MEM, NET, GPU [N (0*)]]\n"
      "                Print Waybar JSON for system\n"
      "                monitoring.\n"
      "                CPU -> CPU Utilization;\n"
      "                MEM -> RAM Usage;\n"
      "                NET -> Network Usage;\n"
      "                GPU -> GPU Usage of GPU N, all GPUs\n"
      "                       in the tooltip.\n"
      "  -k, --keys [CAPS, NUM, SCROLL]\n"
      "                Print Waybar JSON class for each\n"
      "                lock key.\n"
      "                CAPS -> Caps Lock\n"
      "                NUM -> Num Lock\n"
      "                SCROLL -> Scroll Lock\n"
      "  -a, --hotspot Print Waybar JSON for hotspot using\n"
      "                \"create_ap\". (ex. linux-wifi-hotspot)\n"
      "  -u, --updates [MINUTES (30*)] [NOAUR]\n"
      "                Print Waybar JSON for pacman and AUR\n"
      "                updates. Checks every MINUTES and\n"
      "                after every pacman transaction.\n"
      "                NOAUR -> Do not check the AUR.\n"
      "  -u, --updates check\n"
      "                Make the running modules check now.\n"
      "  -V, --volume [OUT*, IN]\n"
      "                Print Waybar JSON for the default\n"
      "                output (speakers/headphones) or\n"
      "                input (microphone) device.\n"
      "  -V, --volume [OUT*, IN] [up, down] [STEP (5*)]\n"
      "  -V, --volume [OUT*, IN] [set] [PERCENT]\n"
      "  -V, --volume [OUT*, IN] [mute, unmute, toggle, next]\n"
      "                Change the volume, mute, or switch\n"
      "                to the next device.\n"
      "  -n, --network Print Waybar JSON for the network\n"
      "                connection (NetworkManager).\n"
      "  -l, --layout  Print Waybar JSON for the Hyprland\n"
      "                layout of the active workspace.\n"
      "  -m, --music [PLAYER[:BINARY]] [LENGTH (40*)]\n"
      "                Print Waybar JSON for the MPRIS player.\n"
      "                PLAYER -> Player to follow (ex. fooyin),\n"
      "                          otherwise the one playing.\n"
      "                BINARY -> Program to start for PLAYER,\n"
      "                          if it is not the same name.\n"
      "                LENGTH -> Maximum length of the text.\n"
      "  -m, --music [PLAYER[:BINARY]] [toggle, play, pause, stop,\n"
      "              next, prev, raise]\n"
      "                Control the player. toggle, play and\n"
      "                raise start PLAYER when not running.\n\n");
}

static int is_opt(const char *arg, const char *short_opt,
                  const char *long_opt) {
  return strcmp(arg, short_opt) == 0 || strcmp(arg, long_opt) == 0;
}

static int is_number(const char *s) {
  if (!s || !*s)
    return 0;
  for (; *s; s++)
    if (*s < '0' || *s > '9')
      return 0;
  return 1;
}

/* Prints a JSON line. Exits once Waybar is gone and stdout is closed. */
static void emit_line(const char *json) {
  if (printf("%s\n", json) < 0)
    exit(0);
}

/* Prints a JSON line from a wc_*_json function and frees it */
static void emit(char *json) {
  if (json)
    emit_line(json);
  free(json);
}

static int print_cb(const char *json, void *userdata) {
  (void)userdata;
  emit_line(json);
  return 0;
}

static void unknown_arg(const char *arg, const char *mode) {
  fprintf(stderr, "Error: Unknown Argument '%s' for '%s'!\n", arg, mode);
}

/* Sleeps until the next multiple of period seconds on the wall clock, so the
 * clock ticks exactly when the second changes instead of drifting.
 * Returns early when interrupted and stop_on_signal is set. */
static void sleep_until_next(int period, int stop_on_signal) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);

  struct timespec next = {
      .tv_sec = (now.tv_sec / period + 1) * period,
      .tv_nsec = 0,
  };
  while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &next, NULL) ==
             EINTR &&
         !stop_on_signal)
    ;
}

static void set_handler(int sig, void (*handler)(int)) {
  struct sigaction sa = {0};
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; // no SA_RESTART: interrupt sleeps and polls
  sigaction(sig, &sa, NULL);
}

/* ------------------------------------------------------------------------ */
/* Running instances, so actions like "-d next" can reach the module       */
/* ------------------------------------------------------------------------ */

static char instance_file[512];

static void instance_dir(char *out, size_t size, const char *module) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime && *runtime)
    snprintf(out, size, "%s/waycentral/%s", runtime, module);
  else
    snprintf(out, size, "/tmp/waycentral-%d/%s", (int)getuid(), module);
}

static void instance_cleanup(void) {
  if (instance_file[0])
    unlink(instance_file);
}

static void on_terminate(int sig) {
  if (instance_file[0])
    unlink(instance_file);
  signal(sig, SIG_DFL);
  raise(sig);
}

static void register_instance(const char *module) {
  char dir[480];
  instance_dir(dir, sizeof(dir), module);

  // mkdir -p
  char tmp[480];
  snprintf(tmp, sizeof(tmp), "%s", dir);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0700);
      *p = '/';
    }
  }
  mkdir(tmp, 0700);

  snprintf(instance_file, sizeof(instance_file), "%s/%d", dir, (int)getpid());
  FILE *f = fopen(instance_file, "w");
  if (!f) {
    instance_file[0] = '\0';
    return;
  }
  fclose(f);

  atexit(instance_cleanup);
  set_handler(SIGTERM, on_terminate);
  set_handler(SIGINT, on_terminate);
  set_handler(SIGHUP, on_terminate);
}

static int is_waycentral(int pid) {
  char path[64], comm[64] = "";
  snprintf(path, sizeof(path), "/proc/%d/comm", pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  if (!fgets(comm, sizeof(comm), f))
    comm[0] = '\0';
  fclose(f);
  return strncmp(comm, "waycentral", 10) == 0;
}

/* Sends sig to every running instance of module, returns how many */
static int signal_instances(const char *module, int sig) {
  char dir[480];
  instance_dir(dir, sizeof(dir), module);

  DIR *d = opendir(dir);
  if (!d)
    return 0;

  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (!is_number(ent->d_name))
      continue;
    int pid = atoi(ent->d_name);
    if (is_waycentral(pid) && kill(pid, sig) == 0) {
      count++;
    } else {
      char path[800];
      snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
      unlink(path); // stale
    }
  }
  closedir(d);
  return count;
}

static int signal_or_fail(const char *module, int sig) {
  if (signal_instances(module, sig) > 0)
    return 0;
  fprintf(stderr, "Error: No running '%s' module found!\n", module);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* Modules                                                                  */
/* ------------------------------------------------------------------------ */

static int run_clock(int argc, char **argv, const char *mode) {
  enum wc_clock_format format = WC_CLOCK_12H;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "24H") == 0)
      format = WC_CLOCK_24H;
    else if (strcmp(argv[i], "12AP") == 0)
      format = WC_CLOCK_12H_AMPM;
    else if (strcmp(argv[i], "12H") == 0)
      format = WC_CLOCK_12H;
    else
      return unknown_arg(argv[i], mode), 1;
  }

  for (;;) {
    emit(wc_clock_json(format));
    sleep_until_next(1, 0);
  }
}

static volatile sig_atomic_t date_shift = 0;
static volatile sig_atomic_t date_reset = 0;

static void on_date_next(int sig) {
  (void)sig;
  date_shift++;
}

static void on_date_prev(int sig) {
  (void)sig;
  date_shift--;
}

static void on_date_today(int sig) {
  (void)sig;
  date_reset = 1;
}

static int run_date(int argc, char **argv, const char *mode) {
  enum wc_date_format format = WC_DATE_YMD;
  enum wc_week_start start = WC_WEEK_MONDAY;

  for (int i = 0; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "next") == 0)
      return signal_or_fail("date", SIGUSR1);
    else if (strcmp(a, "prev") == 0)
      return signal_or_fail("date", SIGUSR2);
    else if (strcmp(a, "today") == 0)
      return signal_or_fail("date", SIGRTMIN);
    else if (strcmp(a, "YMD") == 0)
      format = WC_DATE_YMD;
    else if (strcmp(a, "DMY") == 0)
      format = WC_DATE_DMY;
    else if (strcmp(a, "MDY") == 0)
      format = WC_DATE_MDY;
    else if (strcmp(a, "M") == 0)
      start = WC_WEEK_MONDAY;
    else if (strcmp(a, "S") == 0)
      start = WC_WEEK_SUNDAY;
    else
      return unknown_arg(a, mode), 1;
  }

  register_instance("date");
  set_handler(SIGUSR1, on_date_next);
  set_handler(SIGUSR2, on_date_prev);
  set_handler(SIGRTMIN, on_date_today);

  int offset = 0;
  time_t last_nav = 0;
  for (;;) {
    if (date_reset) {
      date_reset = 0;
      offset = 0;
    }
    if (date_shift) {
      offset += date_shift;
      date_shift = 0;
      last_nav = time(NULL);
    }
    // Back to the current month a while after the last scroll
    if (offset != 0 && time(NULL) - last_nav >= DATE_RESET_S)
      offset = 0;

    emit(wc_date_json_offset(format, start, offset));
    sleep_until_next(60, 1);
  }
}

static _Noreturn void run_battery(void) {
  for (;;) {
    emit(wc_battery_json());
    sleep(5);
  }
}

static int run_sysmon(int argc, char **argv, const char *mode) {
  if (argc < 1) {
    fprintf(stderr, "Error: Not Enough Arguments!\n");
    return 1;
  }

  int type, gpu = 0, extra = 1;
  if (strcmp(argv[0], "GPU") == 0 && argc > 1 && is_number(argv[1])) {
    gpu = atoi(argv[1]);
    extra = 2;
  }
  if (argc > extra)
    return unknown_arg(argv[extra], mode), 1;

  if (strcmp(argv[0], "CPU") == 0)
    type = 0;
  else if (strcmp(argv[0], "MEM") == 0)
    type = 1;
  else if (strcmp(argv[0], "NET") == 0)
    type = 2;
  else if (strcmp(argv[0], "GPU") == 0)
    type = 3;
  else
    return unknown_arg(argv[0], mode), 1;

  for (;;) {
    if (type == 0)
      emit(wc_cpu_json(1000));
    else if (type == 1)
      emit(wc_mem_json());
    else if (type == 2)
      emit(wc_net_json(1000));
    else
      emit(wc_gpu_json(gpu, 1000));
    sleep(5);
  }
}

static int key_state(enum wc_lock_key key, const struct wc_lock_keys *keys) {
  return (key == WC_KEY_CAPS)  ? keys->caps
         : (key == WC_KEY_NUM) ? keys->num
                               : keys->scroll;
}

/* Prints the key state whenever it changes, without polling */
static int run_keys(int argc, char **argv, const char *mode) {
  if (argc < 1) {
    fprintf(stderr, "Error: Not Enough Arguments!\n");
    return 1;
  }

  enum wc_lock_key key;
  if (strcmp(argv[0], "CAPS") == 0)
    key = WC_KEY_CAPS;
  else if (strcmp(argv[0], "NUM") == 0)
    key = WC_KEY_NUM;
  else if (strcmp(argv[0], "SCROLL") == 0)
    key = WC_KEY_SCROLL;
  else
    return unknown_arg(argv[0], mode), 1;

  struct wc_lock_monitor *mon = wc_lock_monitor_open();
  if (!mon)
    return 1;

  struct wc_lock_keys keys = {-1, -1, -1};
  int last = -2;

  for (;;) {
    int r = wc_lock_monitor_wait(mon, &keys, -1);
    if (r < 0) {
      // No keyboard readable yet, report it once and retry
      if (last != -1)
        emit(wc_lock_key_json_from(key, NULL));
      last = -1;
      keys = (struct wc_lock_keys){-1, -1, -1};
      sleep(1);
    } else if (key_state(key, &keys) != last) {
      last = key_state(key, &keys);
      emit(wc_lock_key_json_from(key, &keys));
    }
  }
}

static _Noreturn void run_hotspot(void) {
  for (;;) {
    emit(wc_hotspot_json());
    sleep(1);
  }
}

static void on_check_now(int sig) { (void)sig; }

static int run_updates(int argc, char **argv, const char *mode) {
  int interval = 30, aur = 1;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "check") == 0)
      return signal_or_fail("updates", SIGUSR1);
    else if (strcmp(argv[i], "NOAUR") == 0)
      aur = 0;
    else if (is_number(argv[i]) && atoi(argv[i]) > 0)
      interval = atoi(argv[i]);
    else
      return unknown_arg(argv[i], mode), 1;
  }

  register_instance("updates");
  set_handler(SIGUSR1, on_check_now); // interrupts the wait: check now
  return wc_updates_watch(interval, aur, print_cb, NULL) == 0 ? 0 : 1;
}

static int run_volume(int argc, char **argv, const char *mode) {
  enum wc_audio_dir dir = WC_AUDIO_OUTPUT;
  int i = 0;

  if (i < argc && strcmp(argv[i], "OUT") == 0)
    i++;
  else if (i < argc && strcmp(argv[i], "IN") == 0) {
    dir = WC_AUDIO_INPUT;
    i++;
  }

  if (i == argc)
    return wc_audio_watch(dir, print_cb, NULL) == 0 ? 0 : 1;

  const char *action = argv[i++];
  const char *value = (i < argc) ? argv[i++] : NULL;
  if (i < argc || (value && !is_number(value)))
    return unknown_arg(i < argc ? argv[i] : value, mode), 1;

  int amount = value ? atoi(value) : 5;
  enum wc_audio_action act;
  if (strcmp(action, "up") == 0)
    act = WC_AUDIO_RAISE;
  else if (strcmp(action, "down") == 0)
    act = WC_AUDIO_LOWER;
  else if (strcmp(action, "set") == 0 && value)
    act = WC_AUDIO_SET;
  else if (strcmp(action, "mute") == 0)
    act = WC_AUDIO_MUTE;
  else if (strcmp(action, "unmute") == 0)
    act = WC_AUDIO_UNMUTE;
  else if (strcmp(action, "toggle") == 0)
    act = WC_AUDIO_MUTE_TOGGLE;
  else if (strcmp(action, "next") == 0)
    act = WC_AUDIO_NEXT_DEVICE;
  else
    return unknown_arg(action, mode), 1;

  if (wc_audio_control(dir, act, amount) != 0) {
    fprintf(stderr, "Error: Could not change the %s device!\n",
            dir == WC_AUDIO_OUTPUT ? "output" : "input");
    return 1;
  }
  return 0;
}

static int run_network(int argc, char **argv, const char *mode) {
  if (argc > 0)
    return unknown_arg(argv[0], mode), 1;
  return wc_network_watch(print_cb, NULL) == 0 ? 0 : 1;
}

static int run_layout(int argc, char **argv, const char *mode) {
  if (argc > 0)
    return unknown_arg(argv[0], mode), 1;
  return wc_hypr_layout_watch(print_cb, NULL) == 0 ? 0 : 1;
}

static int run_music(int argc, char **argv, const char *mode) {
  static const struct {
    const char *name;
    enum wc_music_action action;
  } actions[] = {
      {"toggle", WC_MUSIC_PLAY_PAUSE}, {"play", WC_MUSIC_PLAY},
      {"pause", WC_MUSIC_PAUSE},       {"stop", WC_MUSIC_STOP},
      {"next", WC_MUSIC_NEXT},         {"prev", WC_MUSIC_PREVIOUS},
      {"raise", WC_MUSIC_RAISE},
  };

  char player[256] = "", binary[256] = "";
  int length = MUSIC_LENGTH, action = -1;

  for (int i = 0; i < argc; i++) {
    int found = 0;
    for (size_t j = 0; j < sizeof(actions) / sizeof(actions[0]); j++) {
      if (strcmp(argv[i], actions[j].name) == 0) {
        action = (int)actions[j].action;
        found = 1;
      }
    }
    if (found)
      continue;

    if (is_number(argv[i])) {
      length = atoi(argv[i]);
    } else if (!player[0]) {
      // PLAYER or PLAYER:BINARY
      snprintf(player, sizeof(player), "%s", argv[i]);
      char *colon = strchr(player, ':');
      if (colon) {
        *colon = '\0';
        snprintf(binary, sizeof(binary), "%s", colon + 1);
      } else {
        snprintf(binary, sizeof(binary), "%s", player);
      }
    } else {
      return unknown_arg(argv[i], mode), 1;
    }
  }

  const char *preferred = player[0] ? player : NULL;

  if (action < 0)
    return wc_music_watch(preferred, length, print_cb, NULL) == 0 ? 0 : 1;

  if (wc_music_control(preferred, (enum wc_music_action)action) == 0)
    return 0;

  // No such player: start it for the actions that mean "I want music"
  if (binary[0] && (action == WC_MUSIC_PLAY_PAUSE ||
                    action == WC_MUSIC_PLAY || action == WC_MUSIC_RAISE)) {
    if (wc_music_launch(binary) == 0)
      return 0;
    fprintf(stderr, "Error: Could not start '%s'!\n", binary);
    return 1;
  }

  fprintf(stderr, "Error: No player found!\n");
  return 1;
}

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);
  signal(SIGPIPE, SIG_IGN); // a closed stdout makes printf fail instead

  if (argc < 2) {
    help_message();
    return 0;
  }

  const char *mode = argv[1];
  int rest = argc - 2;
  char **args = argv + 2;

  if (is_opt(mode, "-h", "--help")) {
    help_message();
    return 0;
  } else if (is_opt(mode, "-v", "--version")) {
    printf("waycentral %s\n", wc_version());
    return 0;
  } else if (is_opt(mode, "-c", "--clock")) {
    return run_clock(rest, args, mode);
  } else if (is_opt(mode, "-d", "--date")) {
    return run_date(rest, args, mode);
  } else if (is_opt(mode, "-b", "--battery")) {
    run_battery();
  } else if (is_opt(mode, "-s", "--sysmon")) {
    return run_sysmon(rest, args, mode);
  } else if (is_opt(mode, "-k", "--keys")) {
    return run_keys(rest, args, mode);
  } else if (is_opt(mode, "-a", "--hotspot")) {
    run_hotspot();
  } else if (is_opt(mode, "-u", "--updates")) {
    return run_updates(rest, args, mode);
  } else if (is_opt(mode, "-V", "--volume")) {
    return run_volume(rest, args, mode);
  } else if (is_opt(mode, "-n", "--network")) {
    return run_network(rest, args, mode);
  } else if (is_opt(mode, "-l", "--layout")) {
    return run_layout(rest, args, mode);
  } else if (is_opt(mode, "-m", "--music")) {
    return run_music(rest, args, mode);
  }

  fprintf(stderr, "Error: Unknown Option '%s'!\n\n", mode);
  help_message();
  return 1;
}
