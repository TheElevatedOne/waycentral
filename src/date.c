#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *month_names[13] = {
    "",     "January", "February",  "March",   "April",    "May",     "June",
    "July", "August",  "September", "October", "November", "December"};

struct calendar_t {
  char header[32];     // "November 2025"
  char weekdays[7][3]; // e.g. {"Mo","Tu","We","Th","Fr","Sa","Su"}
  int rows;            // number of week rows actually used
  char days[6][7][3];  // "01".."31" or ""
};

int is_leap(int y) { return (y % 400 == 0) || (y % 4 == 0 && y % 100 != 0); }

int days_in_month(int m, int y) {
  static const int mdays[] = {0,  31, 28, 31, 30, 31, 30,
                              31, 31, 30, 31, 30, 31};
  return (m == 2 && is_leap(y)) ? 29 : mdays[m];
}

/* Sakamoto's algorithm: returns 0=Sunday,1=Monday,...6=Saturday */
int weekday(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3)
    y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* start_day = 0: Sunday, 1: Monday */
struct calendar_t *make_calendar_month(int month, int year, int start_day) {
  struct calendar_t *cal = malloc(sizeof(struct calendar_t));

  // --- header ---
  snprintf(cal->header, sizeof(cal->header), "%s %d", month_names[month], year);

  // --- weekday labels ---
  const char *names_sun[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
  const char *names_mon[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
  const char **src = (start_day == 1) ? names_mon : names_sun;
  for (int i = 0; i < 7; ++i)
    strcpy(cal->weekdays[i], src[i]);

  int dim = days_in_month(month, year);
  int start_wd = weekday(year, month, 1); // 0 = Sunday
  if (start_day == 1)
    start_wd = (start_wd + 6) % 7; // shift to Monday=0

  // --- fill days matrix ---
  int week = 0;
  int col = start_wd;
  for (int d = 1; d <= dim; ++d) {
    snprintf(cal->days[week][col], 3, "%02d", d);
    col++;
    if (col == 7) {
      col = 0;
      week++;
    }
  }

  cal->rows = week + (col > 0 ? 1 : 0);
  return cal;
}

static char *get_calendar(const int type) {
  time_t today = time(NULL);
  struct tm *local = malloc(sizeof(struct tm));
  localtime_r(&today, local);

  struct calendar_t *cal =
      make_calendar_month(local->tm_mon + 1, local->tm_year + 1900, type);

  char *calendar = malloc(512);
  sprintf(calendar, "");

  int maxlen = 20;
  int monlen = strlen(cal->header);
  int padding;

  if ((maxlen - monlen) % 2 == 0) {
    padding = (maxlen - monlen) / 2;
  } else {
    padding = ((maxlen - monlen) / 2) + 1;
  }

  for (int i = 0; i < padding; i++) {
    sprintf(calendar, "%s ", calendar);
  }
  sprintf(calendar, "%s<span font_weight='bold'>%s</span>\\n\\n", calendar,
          cal->header);

  /* Getting Weekdays Names */
  for (int i = 0; i < 7; i++) {
    sprintf(calendar, "%s%s", calendar, cal->weekdays[i]);
    if (i != 6) {
      sprintf(calendar, "%s ", calendar);
    } else {
      sprintf(calendar, "%s\\n", calendar);
    }
  }

  /* Getting Days */
  for (int i = 0; i < cal->rows; i++) {
    for (int j = 0; j < 7; j++) {
      if (strlen(cal->days[i][j]) <= 2 && 47 < cal->days[i][j][0] &&
          52 > cal->days[i][j][0]) {
        char *day = malloc(64);
        if (atoi(cal->days[i][j]) == local->tm_mday) {
          sprintf(day, "<span font_weight='bold'>%s</span>", cal->days[i][j]);
        } else {
          sprintf(day, "<span font_weight='ultralight'>%s</span>",
                  cal->days[i][j]);
        }
        if (j != 6) {
          sprintf(calendar, "%s%s ", calendar, day);
        } else {
          sprintf(calendar, "%s%s", calendar, day);
        }
      } else {
        sprintf(calendar, "%s   ", calendar);
      }
    }
    if (i != (cal->rows - 1)) {
      sprintf(calendar, "%s\\n", calendar);
    } else {
      sprintf(calendar, "%s", calendar);
    }
  }

  return calendar;
}

static char *get_date(const int type) {
  time_t today = time(NULL);
  struct tm *local = malloc(sizeof(struct tm));
  localtime_r(&today, local);

  char *date_ = malloc(128);
  if (type == 0) {
    strftime(date_, 128, "%y/%m/%d", local);
  } else if (type == 1) {
    strftime(date_, 128, "%d/%m/%y", local);
  } else if (type == 2) {
    strftime(date_, 128, "%m/%d/%y", local);
  } else {
    sprintf(date_, "NULL");
  }

  return date_;
}

void get_cal(const int clock_type, const int cal_type) {
  char *date = get_date(clock_type);
  char *cal = get_calendar(cal_type);

  char *json = malloc(2048);
  sprintf(json, "{\"text\":\"%s\",\"tooltip\":\"%s\"}", date, cal);
  free(date);
  free(cal);
  printf("%s\n", json);
  free(json);
}
