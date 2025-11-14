#include "src/batt.h"
#include "src/clock.h"
#include "src/date.h"
#include "src/sysmon.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct arguments {
  bool clock;
  bool date;
  bool battery;
  bool sysmon;
  bool help;
};

struct sysmon {
  bool cpu;
  bool gpu;
  bool memory;
  bool network;
};

static void help_message() {
  printf("> waycentral [-h/--help] [OPTIONS]\n\nCentralized Program for Waybar "
         "Modules\n\nHELP:\n  -h, --help    Show this help message\n  -v, "
         "--version Show version\n  Defaults are MY "
         "prefercences (shown with a *).\n\nOPTIONS:\n  -c, --clock [24H, "
         "12AP, 12H*]\n "
         "               Print Waybar JSON for clock.\n                24H -> "
         "24 Hour clock;\n                12AP -> 12 Hour clock with AM/PM;\n  "
         "              12H -> 12 Hour clock without AM/PM.\n  -d, --date "
         "[DMY, MDY, YMD*] [M*, S]\n                Print Waybar JSON for "
         "date.\n       "
         "         DMY -> Day/Month/Year;\n                MDY -> "
         "Month/Day/Year;\n                YMD -> Year/Month/Day.\n            "
         "    M -> Monday First in Calendar Tooltip;\n                S -> "
         "Sunday Fist in Calendar Tooltip.\n  -b, "
         "--battery Print Waybar JSON for battery.\n  -s, --sysmon [CPU, MEM, "
         "NET]\n                Print Waybar JSON for system\n                "
         "monitoring.\n                CPU -> CPU Utilization;\n               "
         " MEM -> RAM Usage;\n                NET -> Network Usage.\n\n");
}

int main(int argc, char *argv[]) {
  struct arguments *args = malloc(sizeof(struct arguments));
  setbuf(stdout, NULL);

  for (int i = 0; i < argc; i++) {
    if (strcmp("-c", argv[i]) == 0 || strcmp("--clock", argv[i]) == 0) {
      args->clock = true;
    } else if (strcmp("-d", argv[i]) == 0 || strcmp("--date", argv[i]) == 0) {
      args->date = true;
    } else if (strcmp("-b", argv[i]) == 0 ||
               strcmp("--battery", argv[i]) == 0) {
      args->battery = true;
    } else if (strcmp("-s", argv[i]) == 0 || strcmp("--sysmon", argv[i]) == 0) {
      args->sysmon = true;
    } else {
      args->help = true;
    }
  }

  if (args->clock) {
    for (int i = 1; i < argc; i++) {
      if (argc == 2) {
        while (true) {
          get_clock(2);
          sleep(1);
        }
      } else if (strcmp("24H", argv[i]) == 0) {
        while (true) {
          get_clock(0);
          sleep(1);
        }
      } else if (strcmp("12H", argv[i]) == 0) {
        while (true) {
          get_clock(2);
          sleep(1);
        }
      } else if (strcmp("12AP", argv[i]) == 0) {
        while (true) {
          get_clock(1);
          sleep(1);
        }
      }
    }
  } else if (args->date) {
    int weekday = 1;
    for (int i = 1; i < argc; i++) {
      if (strcmp("M", argv[i]) == 0) {
        weekday = 1;
      } else if (strcmp("S", argv[i]) == 0) {
        weekday = 0;
      }
    }
    for (int i = 1; i < argc; i++) {
      if (argc == 2) {
        while (true) {
          get_cal(0, weekday);
          sleep(15);
        }
      } else if (strcmp("YMD", argv[i]) == 0) {
        while (true) {
          get_cal(0, weekday);
          sleep(15);
        }
      } else if (strcmp("DMY", argv[i]) == 0) {
        while (true) {
          get_cal(1, weekday);
          sleep(15);
        }
      } else if (strcmp("MDY", argv[i]) == 0) {
        while (true) {
          get_cal(2, weekday);
          sleep(15);
        }
      }
    }
  } else if (args->battery) {
    while (true) {
      format_batt();
      sleep(5);
    }
  } else if (args->sysmon) {
    for (int i = 1; i < argc; i++) {
      if (argc == 2) {
        printf("Error: Not Enough Arguments!\n");
        return 1;
      } else if (strcmp("CPU", argv[i]) == 0) {
        while (true) {
          system_monitor(0);
          sleep(5);
        }
      } else if (strcmp("MEM", argv[i]) == 0) {
        while (true) {
          system_monitor(1);
          sleep(5);
        }
      } else if (strcmp("NET", argv[i]) == 0) {
        while (true) {
          system_monitor(2);
          sleep(5);
        }
      }
    }
  } else {
    help_message();
  }
  return 0;
}
