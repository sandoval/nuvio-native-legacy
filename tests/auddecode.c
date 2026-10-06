#include "../src/auddecode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
static void *cancel(void *p) { usleep(100000);auddecode_cancel(p);return NULL; }
int main(int argc,char **argv) {
  assert(argc==6);assert(auddecode_available());
  AudDecode *d=auddecode_create();assert(d);
  int track=atoi(argv[2]);double start=atof(argv[3]);
  pthread_t thread;int cancellation=!strcmp(argv[5],"cancel");
  if(cancellation) pthread_create(&thread,NULL,cancel,d);
  int status=auddecode_open(d,argv[1],"Authorization: Bearer engine-test\nX-Fixture: yes",track,start,2);
  if(cancellation) { pthread_join(thread,NULL);assert(status<0);assert(strstr(auddecode_error(d),"cancel")); }
  else if(!strcmp(argv[5],"reject")) { assert(status<0);fprintf(stderr,"Expected rejection: %s\n",auddecode_error(d)); }
  else {
    if(status<0) { fprintf(stderr,"Open error: %s\n",auddecode_error(d));return 1; }
    FILE *f=fopen(argv[4],"wb");assert(f);
    AudDecodePcm pcm;int64_t prev=-1;int samples=0;
    while((status=auddecode_next(d,&pcm))>0) {
      assert(pcm.count>0 && pcm.count<=32768);
      assert(pcm.pts_us>=start*1000000-63);
      if(prev>=0) assert(llabs(pcm.pts_us-prev)<=63);
      fprintf(stdout,"%lld %d\n",(long long)pcm.pts_us,pcm.count);
      fwrite(pcm.samples,sizeof(int16_t),pcm.count,f);samples+=pcm.count;
      prev=pcm.pts_us+(int64_t)pcm.count*1000000/16000;
    }
    fclose(f);
    if(status<0) { fprintf(stderr,"Decode error: %s\n",auddecode_error(d));return 1; }
    assert(samples==32000);
  }
  AudDecodeMetrics m;auddecode_metrics(d,&m);assert(m.source_bytes<=64u*1024u*1024u);
  assert(m.elapsed_seconds<120.1);
  auddecode_destroy(d);return 0;
}
