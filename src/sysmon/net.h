#ifndef NET_H_
#define NET_H_

struct netstat_t {
  unsigned long long upload;
  unsigned long long download;
  unsigned long long up_speed;
  unsigned long long down_speed;
};

struct netstat_t *get_network_stats(int interval_ms);

#endif // !NET_H_
