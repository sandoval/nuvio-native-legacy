#ifndef NV_AUDPLATFORM_H
#define NV_AUDPLATFORM_H
#include <stdio.h>
/* The webOS SDK libc is 2.12: getauxval (2.16) cannot be a loader dependency.
 * Linux exposes the same process capability data via this finite local read. */
static inline unsigned long audplatform_auxv(unsigned long type) {
  FILE *f=fopen("/proc/self/auxv","rb");
  unsigned long entry[2], value=0;
  if(!f) return 0;
  for(int i=0;i<128 && fread(entry,sizeof(entry),1,f)==1;i++) {
    if(entry[0]==type) { value=entry[1]; break; }
    if(entry[0]==0) break;
  }
  fclose(f); return value;
}
#endif
