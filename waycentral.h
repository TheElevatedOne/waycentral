#ifndef WAYCENTRAL_H_
#define WAYCENTRAL_H_

/*
 * libwaycentral - the Waybar modules of waycentral as a C library.
 *
 * Link with -lwaycentral (or `pkg-config --libs waycentral`).
 *
 * Two kinds of functions are provided:
 *   - wc_*_get / wc_*_list fill plain structs with raw values, for programs
 *     that want to format the data themselves.
 *   - wc_*_json return a heap allocated Waybar JSON line (without the
 *     trailing newline), exactly what the waycentral binary prints.
 *     Release it with free(). NULL is returned on allocation failure.
 *
 * Event driven modules also have wc_*_watch functions. They block, call the
 * callback with a new JSON line whenever the output changes and return when
 * the callback returns non-zero (0) or on a fatal error (-1).
 *
 * Extra dependencies: libalpm + libcurl + json-c (updates), libpulse (audio),
 * libsystemd (network, music), json-c (Hyprland).
 */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define WC_API __attribute__((visibility("default")))
#else
#define WC_API
#endif

/* ------------------------------------------------------------------------ */
/* General                                                                  */
/* ------------------------------------------------------------------------ */

/* Version of the library, e.g. "1.2.0". */
WC_API const char *wc_version(void);

/* Receives every new JSON line of a wc_*_watch function.
 * Return 0 to keep watching, anything else to stop. */
typedef int (*wc_json_cb)(const char *json, void *userdata);

/* ------------------------------------------------------------------------ */
/* Clock                                                                    */
/* ------------------------------------------------------------------------ */

enum wc_clock_format {
  WC_CLOCK_24H = 0,      /* 23:10:18    */
  WC_CLOCK_12H_AMPM = 1, /* 11:10:18 PM */
  WC_CLOCK_12H = 2,      /* 11:10:18    */
};

/* Seconds since boot, or -1 on error. */
WC_API long wc_uptime(void);

/* Days since the birth of the root filesystem (system install), or -1 if the
 * filesystem does not record a birth time. */
WC_API long wc_system_age_days(void);

/* {"text":"<time>","tooltip":"Uptime:\t...\nBirth:\t..."} */
WC_API char *wc_clock_json(enum wc_clock_format format);

/* ------------------------------------------------------------------------ */
/* Date / Calendar                                                          */
/* ------------------------------------------------------------------------ */

enum wc_date_format {
  WC_DATE_YMD = 0, /* yy/mm/dd */
  WC_DATE_DMY = 1, /* dd/mm/yy */
  WC_DATE_MDY = 2, /* mm/dd/yy */
};

enum wc_week_start {
  WC_WEEK_SUNDAY = 0,
  WC_WEEK_MONDAY = 1,
};

struct wc_calendar {
  char header[32];     /* "November 2025"                    */
  char weekdays[7][3]; /* "Mo", "Tu", ... in display order   */
  int rows;            /* number of week rows used (4-6)     */
  int days[6][7];      /* day of month, 0 for an empty cell  */
};

/* Fills a month grid. month is 1-12. Returns 0, or -1 on invalid input. */
WC_API int wc_calendar_make(struct wc_calendar *out, int month, int year,
                            enum wc_week_start start);

/* {"text":"<date>","tooltip":"<pango calendar of the current month>"} */
WC_API char *wc_date_json(enum wc_date_format format,
                          enum wc_week_start start);

/* Same, but the tooltip shows the month month_offset months away from the
 * current one (negative for the past). */
WC_API char *wc_date_json_offset(enum wc_date_format format,
                                 enum wc_week_start start, int month_offset);

/* ------------------------------------------------------------------------ */
/* Battery                                                                  */
/* ------------------------------------------------------------------------ */

#define WC_MAX_BATTERIES 8

/* Values as reported by /sys/class/power_supply/BAT* (µV, µA, µWh/µAh). */
struct wc_battery {
  char name[64];
  int capacity; /* percent */
  char status[32];
  long full;
  long full_design;
  long voltage_now;
  long current_now;
  char model_name[128];
  char manufacturer[128];
  char technology[64];
};

/* Fills up to max batteries. Returns how many were found, or -1 on error. */
WC_API int wc_battery_list(struct wc_battery *out, int max);

/* {"text":"<capacity>% <icon>","tooltip":"..."}
 * Without a battery: {"text":"","tooltip":"No Battery","class":"none"} */
WC_API char *wc_battery_json(void);

/* ------------------------------------------------------------------------ */
/* System Monitoring                                                        */
/* ------------------------------------------------------------------------ */

struct wc_cpu_usage {
  int count;  /* number of entries in usage                   */
  int *usage; /* percent; [0] is the total, [1..] every core  */
};

/* Samples /proc/stat twice, interval_ms apart (blocks for that long).
 * Release with wc_cpu_usage_free(). Returns NULL on error. */
WC_API struct wc_cpu_usage *wc_cpu_usage_get(int interval_ms);
WC_API void wc_cpu_usage_free(struct wc_cpu_usage *usage);

/* All values in kiB. */
struct wc_meminfo {
  unsigned long long mem_total;
  unsigned long long mem_free;
  unsigned long long mem_available;
  unsigned long long mem_used;
  unsigned long long buffers;
  unsigned long long cached;
  unsigned long long shared;
  unsigned long long swap_total;
  unsigned long long swap_free;
  unsigned long long swap_used;
};

/* Reads /proc/meminfo. Returns 0, or -1 on error. */
WC_API int wc_meminfo_get(struct wc_meminfo *out);

/* Totals in bytes since boot, speeds in bytes per second. Loopback and
 * virtual interfaces (docker, veth, br-, vmnet, tun, tap) are ignored. */
struct wc_netstat {
  unsigned long long upload;
  unsigned long long download;
  unsigned long long up_speed;
  unsigned long long down_speed;
};

/* Samples /proc/net/dev twice, interval_ms apart (blocks for that long).
 * Returns 0, or -1 on error. */
WC_API int wc_netstat_get(struct wc_netstat *out, int interval_ms);

/* The *_json functions block for interval_ms while sampling. */
WC_API char *wc_cpu_json(int interval_ms);
WC_API char *wc_mem_json(void);
WC_API char *wc_net_json(int interval_ms);

/* ------------------------------------------------------------------------ */
/* Lock Keys                                                                */
/* ------------------------------------------------------------------------ */

enum wc_lock_key {
  WC_KEY_CAPS = 0,
  WC_KEY_NUM = 1,
  WC_KEY_SCROLL = 2,
};

struct wc_lock_keys {
  int caps;
  int num;
  int scroll;
};

/* Reads the LED state of the first keyboard in /dev/input (needs read access,
 * usually the "input" group). Returns 0, or -1 if no keyboard is readable. */
WC_API int wc_lock_keys_get(struct wc_lock_keys *out);

/* {"class":"on|off","tooltip":"Capslock On|Off"}
 * Without a readable keyboard the class is "unknown". */
WC_API char *wc_lock_key_json(enum wc_lock_key key);

/* Event driven alternative to polling wc_lock_keys_get(): keeps every
 * keyboard in /dev/input open and wakes up as soon as a lock key changes. */
struct wc_lock_monitor;

/* Returns NULL on allocation failure. Keyboards are (re)scanned on demand,
 * so it is fine if none are present yet. */
WC_API struct wc_lock_monitor *wc_lock_monitor_open(void);
WC_API void wc_lock_monitor_close(struct wc_lock_monitor *mon);

/* Blocks until the lock key state differs from *state or timeout_ms passes
 * (-1 waits forever). On return *state holds the current state.
 * Returns 1 if it changed, 0 on timeout, -1 if no keyboard is readable. */
WC_API int wc_lock_monitor_wait(struct wc_lock_monitor *mon,
                                struct wc_lock_keys *state, int timeout_ms);

/* JSON for one key from an already known state (see wc_lock_key_json). */
WC_API char *wc_lock_key_json_from(enum wc_lock_key key,
                                   const struct wc_lock_keys *state);

/* ------------------------------------------------------------------------ */
/* Hotspot (create_ap / linux-wifi-hotspot)                                 */
/* ------------------------------------------------------------------------ */

struct wc_hotspot {
  int active; /* 1 if a create_ap process is running */
  int hidden;
  char ssid[256];
  char passphrase[256];
  char freq[16];
  char wifi_iface[128];
  char internet_iface[128];
};

/* Fills active, and the rest from /etc/create_ap.conf when active.
 * Returns 0, or -1 on error. */
WC_API int wc_hotspot_get(struct wc_hotspot *out);

/* {"class":"on|off","alt":"on|off","tooltip":"..."} */
WC_API char *wc_hotspot_json(void);


/* ------------------------------------------------------------------------ */
/* Updates (pacman repositories + AUR)                                      */
/* ------------------------------------------------------------------------ */

#define WC_MAX_REPOS 32

struct wc_repo_updates {
  char name[64]; /* sync repository from pacman.conf, e.g. "core" */
  int count;
};

struct wc_updates {
  int total;      /* repositories + AUR                                   */
  int repo_count; /* number of entries in repos, in pacman.conf order    */
  struct wc_repo_updates repos[WC_MAX_REPOS];
  int aur;        /* updates for foreign packages, -1 if not checked      */
  int foreign;    /* installed packages that are in no repository         */
  long checked;   /* unix time of the check                               */
  char error[256]; /* empty, or why the result may be incomplete          */
};

/* Counts available updates without root, like checkupdates: a private copy
 * of the sync databases is kept in $XDG_CACHE_HOME/waycentral/pacman.
 * sync: 1 downloads fresh databases first, 0 reuses the cached copy.
 * aur:  1 also asks the AUR RPC about foreign packages.
 * Returns 0, or -1 if nothing could be counted (see out->error). */
WC_API int wc_updates_check(struct wc_updates *out, int sync, int aur);

/* {"text":"<total>","tooltip":"<count per repository and AUR>",
 *  "class":"updates|none|error","alt":"..."} */
WC_API char *wc_updates_json(const struct wc_updates *updates);

/* Checks every interval_min minutes (30 if <= 0), and again as soon as a
 * pacman transaction finishes. A signal that interrupts the thread (e.g.
 * SIGUSR1 with a handler installed without SA_RESTART) forces a check. */
WC_API int wc_updates_watch(int interval_min, int aur, wc_json_cb cb,
                            void *userdata);

/* ------------------------------------------------------------------------ */
/* Audio (PulseAudio / PipeWire)                                            */
/* ------------------------------------------------------------------------ */

enum wc_audio_dir {
  WC_AUDIO_OUTPUT = 0, /* default sink: speakers, headphones   */
  WC_AUDIO_INPUT = 1,  /* default source: microphone, headset */
};

enum wc_audio_form {
  WC_FORM_UNKNOWN = 0,
  WC_FORM_SPEAKER,
  WC_FORM_HEADPHONES,
  WC_FORM_HEADSET,
  WC_FORM_MICROPHONE,
  WC_FORM_HDMI,
};

struct wc_audio {
  int available; /* 0 if there is no such device             */
  int volume;    /* percent, average of the channels          */
  int muted;
  int bluetooth;
  enum wc_audio_form form;
  char name[256];        /* PulseAudio name                  */
  char description[256]; /* human readable device name       */
  char port[256];        /* active port description, or ""   */
};

enum wc_audio_action {
  WC_AUDIO_SET = 0,      /* volume = value              */
  WC_AUDIO_RAISE,        /* volume += value (max 100)   */
  WC_AUDIO_LOWER,        /* volume -= value             */
  WC_AUDIO_MUTE_TOGGLE,
  WC_AUDIO_MUTE,
  WC_AUDIO_UNMUTE,
  WC_AUDIO_NEXT_DEVICE,  /* make the next device the default */
};

/* Returns 0, or -1 if the sound server is not reachable. */
WC_API int wc_audio_get(enum wc_audio_dir dir, struct wc_audio *out);

/* {"text":"<volume>%","percentage":<volume>,"alt":"<form>[-muted]",
 *  "class":"<form> [muted]","tooltip":"..."}
 * alt is one of speaker, headphones, headset, microphone, hdmi, unknown or
 * none, with -muted appended when muted. */
WC_API char *wc_audio_json(const struct wc_audio *audio, enum wc_audio_dir dir);

/* Prints on every volume, mute or default device change. */
WC_API int wc_audio_watch(enum wc_audio_dir dir, wc_json_cb cb,
                          void *userdata);

/* Returns 0, or -1 on error. */
WC_API int wc_audio_control(enum wc_audio_dir dir, enum wc_audio_action action,
                            int value);

/* ------------------------------------------------------------------------ */
/* Network (NetworkManager, falls back to sysfs)                            */
/* ------------------------------------------------------------------------ */

enum wc_net_type {
  WC_NET_DISCONNECTED = 0,
  WC_NET_ETHERNET,
  WC_NET_WIFI,
  WC_NET_VPN,
  WC_NET_OTHER,
  WC_NET_DISABLED, /* networking or the radio switched off */
};

struct wc_network {
  enum wc_net_type type;
  char connection[256]; /* NetworkManager connection name     */
  char iface[64];
  char ssid[64];
  int signal;    /* wifi signal in percent, -1 if unknown      */
  int frequency; /* wifi frequency in MHz, 0 if unknown        */
  int bitrate;   /* link speed in Mbit/s, 0 if unknown         */
  char ipv4[64];
  int prefix4;
  char ipv6[64];
  char gateway[64];
};

/* Returns 0, or -1 on error. */
WC_API int wc_network_get(struct wc_network *out);

/* {"text":"<ssid|iface>","alt":"wifi|ethernet|vpn|other|disconnected|
 *  disabled","class":"...","percentage":<signal>,"tooltip":"..."} */
WC_API char *wc_network_json(const struct wc_network *net);

/* Prints on every connection change, and every 5 s for the wifi signal. */
WC_API int wc_network_watch(wc_json_cb cb, void *userdata);

/* ------------------------------------------------------------------------ */
/* Hyprland Layout                                                          */
/* ------------------------------------------------------------------------ */

struct wc_hypr_layout {
  char name[64]; /* dwindle, master, scrolling, monocle or a plugin's */
  int workspace;
  char workspace_name[64];
};

/* Asks Hyprland over its IPC socket for the tiled layout of the active
 * workspace. Returns 0, or -1 if Hyprland is not reachable. */
WC_API int wc_hypr_layout_get(struct wc_hypr_layout *out);

/* {"text":"<D|M|S|...>","alt":"<layout>","class":"<layout>",
 *  "tooltip":"Active Layout: <Layout>"} */
WC_API char *wc_hypr_layout_json(const struct wc_hypr_layout *layout);

/* Prints whenever the layout of the active workspace changes. */
WC_API int wc_hypr_layout_watch(wc_json_cb cb, void *userdata);

/* ------------------------------------------------------------------------ */
/* Music (MPRIS)                                                            */
/* ------------------------------------------------------------------------ */

enum wc_music_status {
  WC_MUSIC_NONE = 0, /* no player */
  WC_MUSIC_STOPPED,
  WC_MUSIC_PAUSED,
  WC_MUSIC_PLAYING,
};

struct wc_music {
  enum wc_music_status status;
  char player[128];   /* bus name suffix, e.g. "fooyin" or "spotify" */
  char identity[128]; /* player's display name                      */
  char title[512];
  char artist[512];
  char album[512];
  long long position; /* microseconds, -1 if unknown */
  long long length;   /* microseconds, -1 if unknown */
};

enum wc_music_action {
  WC_MUSIC_PLAY_PAUSE = 0,
  WC_MUSIC_PLAY,
  WC_MUSIC_PAUSE,
  WC_MUSIC_STOP,
  WC_MUSIC_NEXT,
  WC_MUSIC_PREVIOUS,
  WC_MUSIC_RAISE, /* bring the player window up */
};

/* preferred: player to follow (bus name prefix such as "fooyin"), or NULL
 * to follow the playing player, else the first one found. playerctld is
 * skipped unless asked for. Returns 0 (status NONE if there is no player),
 * or -1 if the session bus is not reachable. */
WC_API int wc_music_get(const char *preferred, struct wc_music *out);

/* {"text":"<artist> - <title>","alt":"playing|paused|stopped|none",
 *  "class":"...","tooltip":"..."}
 * The text is cut to max_length characters (no limit if <= 0). */
WC_API char *wc_music_json(const struct wc_music *music, int max_length);

/* Prints on every track, status or player change, and every second while
 * playing to keep the position in the tooltip current. */
WC_API int wc_music_watch(const char *preferred, int max_length,
                          wc_json_cb cb, void *userdata);

/* Returns 0, or -1 if there is no such player or the call failed. */
WC_API int wc_music_control(const char *preferred,
                            enum wc_music_action action);

/* Starts binary (looked up in PATH) detached from the caller.
 * Returns 0, or -1 if it could not be started. */
WC_API int wc_music_launch(const char *binary);

#ifdef __cplusplus
}
#endif

#endif // !WAYCENTRAL_H_
