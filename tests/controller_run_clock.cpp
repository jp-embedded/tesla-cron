// Full-controller test harness only. No production target links this file.
#include <time.h>
extern "C" int nanosleep(const struct timespec *, struct timespec *) { return 0; }
