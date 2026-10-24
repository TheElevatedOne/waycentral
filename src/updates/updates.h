#ifndef UPDATES_H_
#define UPDATES_H_

#include <stddef.h>

/* A foreign package: installed, but in no sync repository */
struct wc_foreign_pkg {
  const char *name;
  const char *version;
};

/* Asks the AUR RPC for newer versions of the given packages.
 * Returns the number of updates, or -1 on error (err is set). */
int wc_aur_count_updates(const struct wc_foreign_pkg *pkgs, size_t count,
                         char *err, size_t err_size);

#endif // !UPDATES_H_
