#ifndef NV_SILERO_RUNTIME_COMPAT_H
#define NV_SILERO_RUNTIME_COMPAT_H
#include <sys/auxv.h>
#include "nuvio_audplatform.h"
/* Linux UAPI include/uapi/linux/auxvec.h: older SDK headers omit this tag. */
#ifndef AT_HWCAP2
#define AT_HWCAP2 26
#endif
/* Preserve CPU capability detection without importing a GLIBC_2.16 symbol. */
#define getauxval audplatform_auxv
#endif
