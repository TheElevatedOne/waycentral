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

/* Calendar date normalized by mktime, noon avoids DST edge cases */
static struct tm make_date(int year, int month, int day) {
  struct tm t = {0};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = 12;
  t.tm_isdst = -1;
  mktime(&t);
  return t;
}

static int same_day(const struct tm *a, const struct tm *b) {
  return a->tm_year == b->tm_year && a->tm_mon == b->tm_mon &&
         a->tm_mday == b->tm_mday;
}

#define CAL_WIDTH 24 /* "Wk" + 2 spaces + 7 columns of "dd " */
/* Pango attributes, used inside format strings */
#define DIM "alpha='45%%'"
#define WEEKEND "alpha='75%%'"

/* Appends a 6 week month grid with ISO week numbers, days of the
 * neighbouring months faded and today highlighted, in a monospace span. */
static void append_calendar(struct wc_buf *buf, const struct tm *today,
                            enum wc_week_start start, int month_offset) {
  // The month to show
  struct tm first = make_date(today->tm_year + 1900,
                              today->tm_mon + 1 + month_offset, 1);
  int year = first.tm_year + 1900;
  int month = first.tm_mon + 1;

  wc_buf_appendf(buf, "<span font_family='monospace'>");

  char header[64];
  strftime(header, sizeof(header), "%B %Y", &first);
  int pad = (CAL_WIDTH - (int)strlen(header)) / 2;
  wc_buf_appendf(buf, "%*s<span weight='bold'>%s</span>\\n\\n",
                 pad > 0 ? pad : 0, "", header);

  static const char *names_sun[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
  static const char *names_mon[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
  const char **names = (start == WC_WEEK_MONDAY) ? names_mon : names_sun;
  // Columns holding Saturday and Sunday
  int sat = (start == WC_WEEK_MONDAY) ? 5 : 6;
  int sun = (start == WC_WEEK_MONDAY) ? 6 : 0;

  wc_buf_appendf(buf, "<span " DIM ">Wk</span> ");
  for (int i = 0; i < 7; i++) {
    if (i == sat || i == sun)
      wc_buf_appendf(buf, " <span weight='bold' " WEEKEND ">%s</span>",
                     names[i]);
    else
      wc_buf_appendf(buf, " <span weight='bold'>%s</span>", names[i]);
  }
  wc_buf_appendf(buf, "\\n");

  // Column of the 1st, then start the grid that many days earlier
  int col = first.tm_wday;
  if (start == WC_WEEK_MONDAY)
    col = (col + 6) % 7;

  for (int row = 0; row < 6; row++) {
    // ISO week of the row's Monday
    int monday_col = (start == WC_WEEK_MONDAY) ? 0 : 1;
    struct tm monday = make_date(year, month, 1 - col + row * 7 + monday_col);
    char week[8];
    strftime(week, sizeof(week), "%V", &monday);
    wc_buf_appendf(buf, "<span " DIM ">%s</span> ", week);

    for (int c = 0; c < 7; c++) {
      struct tm day = make_date(year, month, 1 + row * 7 + c - col);

      if (same_day(&day, today))
        wc_buf_appendf(buf,
                       " <span weight='heavy' underline='single'>%02d</span>",
                       day.tm_mday);
      else if (day.tm_mon != first.tm_mon)
        wc_buf_appendf(buf, " <span " DIM ">%02d</span>", day.tm_mday);
      else if (c == sat || c == sun)
        wc_buf_appendf(buf, " <span " WEEKEND ">%02d</span>", day.tm_mday);
      else
        wc_buf_appendf(buf, " %02d", day.tm_mday);
    }
    wc_buf_appendf(buf, "\\n");
  }

  char footer[96];
  strftime(footer, sizeof(footer), "%A, %d %B %Y", today);
  wc_buf_appendf(buf, "\\n<span weight='bold'>%s</span>", footer);
  strftime(footer, sizeof(footer), "Week %V, Day %j", today);
  wc_buf_appendf(buf, "\\n<span " DIM ">%s</span></span>", footer);
}

char *wc_date_json_offset(enum wc_date_format format, enum wc_week_start start,
                          int month_offset) {
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
  append_calendar(&json, &local, start, month_offset);
  wc_buf_appendf(&json, "\"}");
  return wc_buf_finish(&json);
}

char *wc_date_json(enum wc_date_format format, enum wc_week_start start) {
  return wc_date_json_offset(format, start, 0);
}
