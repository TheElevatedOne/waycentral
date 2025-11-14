#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static char *get_uptime() {
  FILE *file = fopen("/proc/uptime", "r");
  double uptime = 0, idle = 0;
  fscanf(file, "%lf %lf", &uptime, &idle);
  fclose(file);

  long uptime_long = (long)uptime;

  typedef struct {
    int days;
    int hours;
    int minutes;
    int seconds;
  } count_t;

  count_t *ct = malloc(sizeof(count_t));

  ct->days = 0;
  ct->hours = 0;
  ct->minutes = 0;
  ct->seconds = 0;

  if (uptime_long >= (24 * 3600)) {
    ct->days = (int)(uptime_long / (24 * 3600));
    uptime_long %= (24 * 3600);
  }
  if (uptime_long >= 3600) {
    ct->hours = (int)(uptime_long / 3600);
    uptime_long %= 3600;
  }
  if (uptime_long >= 60) {
    ct->minutes = (int)(uptime_long / 60);
    uptime_long %= 60;
  }
  ct->seconds = (int)uptime_long;

  char *value = malloc(128);

  sprintf(value, "");

  if (ct->days != 0) {
    sprintf(value, "%d day/s ", ct->days);
  }

  sprintf(value, "%s%02d:%02d:%02d", value, ct->hours, ct->minutes,
          ct->seconds);

  return value;
}

static char *get_birth() {
  FILE *stat = popen("stat -c %W /", "r");
  long birth;
  long now = (long)time(NULL);
  fscanf(stat, "%lu", &birth);
  pclose(stat);

  long birthday = ((now - birth) / (24 * 3600));

  char *value = malloc(128);
  sprintf(value, "%lu day/s", birthday);

  return value;
}

static char *get_time(const int type) {
  time_t now = time(NULL);
  struct tm *local = malloc(sizeof(struct tm));
  localtime_r(&now, local);

  char *time_ = malloc(128);
  if (type == 0) {
    strftime(time_, 128, "%H:%M:%S", local);
  } else if (type == 1) {
    strftime(time_, 128, "%I:%M:%S %p", local);
  } else if (type == 2) {
    strftime(time_, 128, "%I:%M:%S", local);
  } else {
    sprintf(time_, "NULL");
  }

  return time_;
}

void get_clock(const int type) {
  char *birth = get_birth();
  char *uptime = get_uptime();
  char *clock = get_time(type);

  char *json = malloc(512);
  sprintf(json, "{\"text\":\"%s\",\"tooltip\":\"Uptime:\\t%s\\nBirth:\\t%s\"}",
          clock, uptime, birth);
  free(birth);
  free(clock);
  free(uptime);
  printf("%s\n", json);
  free(json);
}
