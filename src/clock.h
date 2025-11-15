#ifndef CLOCK_H_
#define CLOCK_H_

/* 0 -> 24h
 * 1 -> 12h with AM/PM
 * 2 -> 12h without AM/PM */
void get_clock(const int type);

#endif // !CLOCK_H_
