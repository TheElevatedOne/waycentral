#include "waycentral.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum module {
  MOD_NONE,
  MOD_CLOCK,
  MOD_DATE,
  MOD_BATTERY,
  MOD_SYSMON,
  MOD_KEYS,
  MOD_AP,
};

enum sysmon_type { SYSMON_CPU, SYSMON_MEM, SYSMON_NET };

static void help_message(void) {
  printf("> waycentral [-h/--help] "
         "[OPTIONS]\n\nCentralized Program for Waybar "
         "Modules\n\nHELP:\n  -h, --help    Show this help message\n  -v, "
         "--version Show version\n  Defaults are MY "
         "preferences (shown with a *).\n\nOPTIONS:\n  -c, --clock [24H, "
         "12AP, 12H*]\n "
         "               Print Waybar JSON for clock.\n                24H -> "
         "24 Hour clock;\n                12AP -> 12 Hour clock with AM/PM;\n  "
         "              12H -> 12 Hour clock without AM/PM.\n  -d, --date "
         "[DMY, MDY, YMD*] [M*, S]\n                Print Waybar JSON for "
         "date.\n       "
         "         DMY -> Day/Month/Year;\n                MDY -> "
         "Month/Day/Year;\n                YMD -> Year/Month/Day.\n            "
         "    M -> Monday First in Calendar Tooltip;\n                S -> "
         "Sunday First in Calendar Tooltip.\n  -b, "
         "--battery Print Waybar JSON for battery.\n  -s, --sysmon [CPU, MEM, "
         "NET]\n                Print Waybar JSON for system\n                "
         "monitoring.\n                CPU -> CPU Utilization;\n               "
         " MEM -> RAM Usage;\n                NET -> Network Usage.\n  -k, "
         "--keys [CAPS, NUM, SCROLL]\n                Print Waybar JSON class "
         "for each\n                lock key.\n                CAPS -> Caps "
         "Lock\n                NUM -> Num Lock\n                SCROLL -> "
         "Scroll Lock\n  -a, --hotspot Print Waybar JSON for hotspot using\n   "
         "             \"create_ap\". (ex. linux-wifi-hotspot)\n\n");
}

static int is_opt(const char *arg, const char *short_opt,
                  const char *long_opt) {
  return strcmp(arg, short_opt) == 0 || strcmp(arg, long_opt) == 0;
}

/* Prints a JSON line from a wc_*_json function and frees it */
static void emit(char *json) {
  if (json)
    printf("%s\n", json);
  free(json);
}

int main(int argc, char *argv[]) {
  enum module module = MOD_NONE;
  enum wc_clock_format clock_format = WC_CLOCK_12H;
  enum wc_date_format date_format = WC_DATE_YMD;
  enum wc_week_start week_start = WC_WEEK_MONDAY;
  enum sysmon_type sysmon = SYSMON_CPU;
  enum wc_lock_key key = WC_KEY_CAPS;
  int has_value = 0;

  setbuf(stdout, NULL);

  if (argc < 2) {
    help_message();
    return 0;
  }

  const char *mode = argv[1];
  if (is_opt(mode, "-h", "--help")) {
    help_message();
    return 0;
  } else if (is_opt(mode, "-v", "--version")) {
    printf("waycentral %s\n", wc_version());
    return 0;
  } else if (is_opt(mode, "-c", "--clock")) {
    module = MOD_CLOCK;
  } else if (is_opt(mode, "-d", "--date")) {
    module = MOD_DATE;
  } else if (is_opt(mode, "-b", "--battery")) {
    module = MOD_BATTERY;
  } else if (is_opt(mode, "-s", "--sysmon")) {
    module = MOD_SYSMON;
  } else if (is_opt(mode, "-k", "--keys")) {
    module = MOD_KEYS;
  } else if (is_opt(mode, "-a", "--hotspot")) {
    module = MOD_AP;
  } else {
    fprintf(stderr, "Error: Unknown Option '%s'!\n\n", mode);
    help_message();
    return 1;
  }

  for (int i = 2; i < argc; i++) {
    const char *arg = argv[i];
    int known = 1;

    if (module == MOD_CLOCK && strcmp(arg, "24H") == 0)
      clock_format = WC_CLOCK_24H;
    else if (module == MOD_CLOCK && strcmp(arg, "12AP") == 0)
      clock_format = WC_CLOCK_12H_AMPM;
    else if (module == MOD_CLOCK && strcmp(arg, "12H") == 0)
      clock_format = WC_CLOCK_12H;
    else if (module == MOD_DATE && strcmp(arg, "YMD") == 0)
      date_format = WC_DATE_YMD;
    else if (module == MOD_DATE && strcmp(arg, "DMY") == 0)
      date_format = WC_DATE_DMY;
    else if (module == MOD_DATE && strcmp(arg, "MDY") == 0)
      date_format = WC_DATE_MDY;
    else if (module == MOD_DATE && strcmp(arg, "M") == 0)
      week_start = WC_WEEK_MONDAY;
    else if (module == MOD_DATE && strcmp(arg, "S") == 0)
      week_start = WC_WEEK_SUNDAY;
    else if (module == MOD_SYSMON && strcmp(arg, "CPU") == 0)
      sysmon = SYSMON_CPU;
    else if (module == MOD_SYSMON && strcmp(arg, "MEM") == 0)
      sysmon = SYSMON_MEM;
    else if (module == MOD_SYSMON && strcmp(arg, "NET") == 0)
      sysmon = SYSMON_NET;
    else if (module == MOD_KEYS && strcmp(arg, "CAPS") == 0)
      key = WC_KEY_CAPS;
    else if (module == MOD_KEYS && strcmp(arg, "NUM") == 0)
      key = WC_KEY_NUM;
    else if (module == MOD_KEYS && strcmp(arg, "SCROLL") == 0)
      key = WC_KEY_SCROLL;
    else
      known = 0;

    if (!known) {
      fprintf(stderr, "Error: Unknown Argument '%s' for '%s'!\n", arg, mode);
      return 1;
    }
    has_value = 1;
  }

  if ((module == MOD_SYSMON || module == MOD_KEYS) && !has_value) {
    fprintf(stderr, "Error: Not Enough Arguments!\n");
    return 1;
  }

  while (1) {
    switch (module) {
    case MOD_CLOCK:
      emit(wc_clock_json(clock_format));
      sleep(1);
      break;
    case MOD_DATE:
      emit(wc_date_json(date_format, week_start));
      sleep(15);
      break;
    case MOD_BATTERY:
      emit(wc_battery_json());
      sleep(5);
      break;
    case MOD_SYSMON:
      if (sysmon == SYSMON_CPU)
        emit(wc_cpu_json(1000));
      else if (sysmon == SYSMON_MEM)
        emit(wc_mem_json());
      else
        emit(wc_net_json(1000));
      sleep(5);
      break;
    case MOD_KEYS:
      emit(wc_lock_key_json(key));
      sleep(1);
      break;
    case MOD_AP:
      emit(wc_hotspot_json());
      sleep(1);
      break;
    case MOD_NONE:
      return 1;
    }
  }
}
