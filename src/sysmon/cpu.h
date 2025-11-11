#ifndef CPU_H_
#define CPU_H_

struct cpu_usage_t {
  int count;
  int *usage;
};

struct cpu_usage_t *get_cpu_usage(int interval_ms);

#endif // !CPU_H_
