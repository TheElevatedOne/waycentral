#include "../waycentral.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *month_names[13] = {
    "",     "January", "February",  "March",   "April",    "May",     "June",
    "July", "August",  "September", "October", "November", "December"};

static int is_leap(int y) {
  return (y % 400 == 0) || (y % 4 == 0 && y % 100 != 0);
}

static int days_in_month(int m, int y) {
  static const int mdays[] = {0,  31, 28, 31, 30, 31, 30,
                              31, 31, 30, 31, 30, 31};
  return (m == 2 && is_leap(y)) ? 29 : mdays[m];
}

/* Sakamoto's algorithm: returns 0=Sunday,1=Monday,...6=Saturday */
static int weekday(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3)
    y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

int wc_calendar_make(struct wc_calendar *out, int month, int year,
                     enum wc_week_start start) {
  if (!out || month < 1 || month > 12 || year < 1)
    return -1;

  memset(out, 0, sizeof(*out));

  snprintf(out->header, sizeof(out->header), "%s %d", month_names[month],
           year);

  static const char *names_sun[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
  static const char *names_mon[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
  const char **names = (start == WC_WEEK_MONDAY) ? names_mon : names_sun;
  for (int i = 0; i < 7; ++i)
    memcpy(out->weekdays[i], names[i], 3);

  int dim = days_in_month(month, year);
  int col = weekday(year, month, 1); // 0 = Sunday
  if (start == WC_WEEK_MONDAY)
    col = (col + 6) % 7; // shift to Monday=0

  int week = 0;
  for (int d = 1; d <= dim; ++d) {
    out->days[week][col] = d;
    if (++col == 7) {
      col = 0;
      week++;
    }
  }

  out->rows = week + (col > 0 ? 1 : 0);
  return 0;
}

static void append_calendar(struct wc_buf *buf, const struct tm *local,
                            enum wc_week_start start) {
  struct wc_calendar cal;
  if (wc_calendar_make(&cal, local->tm_mon + 1, local->tm_year + 1900,
                       start) != 0)
    return;

  /* Center the header above the 20 character wide day grid */
  const int width = 20;
  int padding = (width - (int)strlen(cal.header) + 1) / 2;
  wc_buf_appendf(buf, "%*s<span font_weight='bold'>%s</span>\\n\\n",
                 padding > 0 ? padding : 0, "", cal.header);

  for (int i = 0; i < 7; i++)
    wc_buf_appendf(buf, "%s%s", cal.weekdays[i], (i != 6) ? " " : "\\n");

  for (int i = 0; i < cal.rows; i++) {
    for (int j = 0; j < 7; j++) {
      const char *sep = (j != 6) ? " " : "";
      int day = cal.days[i][j];
      if (day == 0)
        wc_buf_appendf(buf, "  %s", sep);
      else
        wc_buf_appendf(buf, "<span font_weight='%s'>%02d</span>%s",
                       (day == local->tm_mday) ? "bold" : "ultralight", day,
                       sep);
    }
    if (i != cal.rows - 1)
      wc_buf_appendf(buf, "\\n");
  }
}

char *wc_date_json(enum wc_date_format format, enum wc_week_start start) {
  time_t today = time(NULL);
  struct tm local;
  localtime_r(&today, &local);

  char date[64];
  switch (format) {
  case WC_DATE_YMD:
    strftime(date, sizeof(date), "%y/%m/%d", &local);
    break;
  case WC_DATE_DMY:
    strftime(date, sizeof(date), "%d/%m/%y", &local);
    break;
  case WC_DATE_MDY:
    strftime(date, sizeof(date), "%m/%d/%y", &local);
    break;
  default:
    snprintf(date, sizeof(date), "NULL");
  }

  struct wc_buf json = {0};
  wc_buf_appendf(&json, "{\"text\":\"%s\",\"tooltip\":\"", date);
  append_calendar(&json, &local, start);
  wc_buf_appendf(&json, "\"}");
  return wc_buf_finish(&json);
}
