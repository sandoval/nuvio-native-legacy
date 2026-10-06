/* Raw mono PCM16 stdin, probabilities and segments stdout. External media only. */
#include "audsilero.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static float first_probability, reset_probability;
static int observations;
static void reset_observer(void *unused,int64_t pts,int n,float p) {
  (void)unused; (void)pts; (void)n; reset_probability=p;
}
static void probability(void *unused,int64_t pts,int n,float p) {
  (void)unused; if(!observations++) first_probability=p; printf("P %lld %d %.9g\n",(long long)pts,n,p);
}
int main(int argc,char **argv) {
  if(argc!=4) return 2;
  char error[256]; AudSilero *v=audsilero_create(argv[1],error,sizeof(error));
  if(!v) { fprintf(stderr,"%s\n",error); return 1; }
  int chunk=atoi(argv[2]); int64_t origin=strtoll(argv[3],NULL,10), samples=0;
  int16_t pcm[4096], first[512]={0}; if(chunk<1 || chunk>4096) { audsilero_destroy(v); return 2; }
  audsilero_observe(v,probability,NULL);
  size_t n; while((n=fread(pcm,sizeof(*pcm),chunk,stdin))) {
    if(samples<512) { size_t take=512-(size_t)samples; if(take>n) take=n; memcpy(first+samples,pcm,take*sizeof(*pcm)); }
    if(audsilero_feed(v,pcm,(int)n,origin+samples*1000000/16000)) goto fail;
    samples+=(int64_t)n;
  }
  if(audsilero_flush(v)) goto fail;
  int count; const AudSeg *seg=audsilero_segments(v,&count);
  for(int i=0;i<count;i++) printf("S %.9f %.9f\n",seg[i].inicio,seg[i].fim);
  audsilero_reset(v);
  if(audsilero_feed(v,pcm,0,origin) || audsilero_flush(v)) goto fail;
  audsilero_segments(v,&count); if(count) goto fail;
  if(samples>=512) {
    audsilero_reset(v); audsilero_observe(v,reset_observer,NULL);
    if(audsilero_feed(v,first,512,origin)) goto fail;
    if(fabsf(reset_probability-first_probability)>1e-7f) goto fail;
    /* Seek discontinuity must reset recurrent/context/segment state too. */
    if(audsilero_feed(v,first,512,origin+100000000)) goto fail;
    if(fabsf(reset_probability-first_probability)>1e-7f) goto fail;
    audsilero_segments(v,&count); if(count) goto fail;
  }
  audsilero_destroy(v); return 0;
fail:
  fprintf(stderr,"%s\n",audsilero_error(v)); audsilero_destroy(v); return 1;
}
