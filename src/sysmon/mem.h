#ifndef MEM_H_
#define MEM_H_

struct meminfo_t {
  unsigned long long mem_total;
  unsigned long long mem_free;
  unsigned long long mem_available;
  unsigned long long buffers;
  unsigned long long cached;
  unsigned long long swap_total;
  unsigned long long swap_free;
  unsigned long long swap_used;
  unsigned long long mem_used;
  unsigned long long shared_mem;
  int mem_percent;
  int swap_percent;
};

struct meminfo_t *read_meminfo();

#endif // !MEM_H_
