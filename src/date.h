#ifndef DATE_H_
#define DATE_H_

// Clock Type:
// 0 -> yy/mm/dd,
// 1 -> dd/mm/yy,
// 2 -> mm/dd/yy;
// Calendar Type:
// 0 -> Sunday First,
// 1 -> Monday First;
void get_cal(const int clock_type, const int cal_type);

#endif // !DATE_H_
