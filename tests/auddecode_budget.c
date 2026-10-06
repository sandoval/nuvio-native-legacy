#include "../src/rede.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static unsigned long now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000UL+t.tv_nsec/1000000UL; }
int main(int argc,char **argv) {
  assert(argc==2);long size=0;int64_t total;int status;volatile int cancel=0;
  RedeRangeBudget b={.max_body_bytes=32768,.deadline_ms=now()+1000};
  char *body=rede_baixar_trecho64_budget(argv[1],"Authorization: Bearer engine-test\nX-Fixture: yes",0,1024*1024-1,&size,&total,&status,&cancel,&b);
  assert(!body && !size);assert(b.body_bytes<=32768 && b.body_bytes>16384);assert(b.requests==1);
  b=(RedeRangeBudget){.max_body_bytes=65536,.deadline_ms=now()-1};
  body=rede_baixar_trecho64_budget(argv[1],"Authorization: Bearer engine-test\nX-Fixture: yes",0,1023,&size,&total,&status,&cancel,&b);
  assert(!body && b.requests==0 && b.body_bytes==0);
  puts("Fetched-body and expired-deadline budgets passed");return 0;
}
