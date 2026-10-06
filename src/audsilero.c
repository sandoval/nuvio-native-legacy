#include "audsilero.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NUVIO_SILERO_ORT
#include <onnxruntime_c_api.h>
#include <dlfcn.h>
#if defined(__arm__) && defined(__linux__)
#include <elf.h>
#include "audplatform.h"
#include <asm/hwcap.h>
#endif
struct AudSilero {
  void *library;
  const OrtApi *api;
  OrtEnv *env;
  OrtSession *session;
  OrtMemoryInfo *memory;
  OrtValue *inputs[3], *outputs[2];
  float input[576], state[256], output_state[256], probability;
  int64_t rate;
  int16_t pending[512];
  int used, started, failed, count, active, flushed;
  int64_t origin, samples;
  double begin, end;
  AudSeg segments[AUDVAD_MAX_SEG];
  char error[256];
  AudSileroProbability observer;
  void *opaque;
};
static int check(AudSilero *v, OrtStatus *s) {
  if (!s) return 0;
  snprintf(v->error, sizeof(v->error), "%s", v->api->GetErrorMessage(s));
  v->api->ReleaseStatus(s); v->failed = 1; return -1;
}
#define TRY(x) do { if (check(v, (x))) goto fail; } while (0)
static int shape(AudSilero *v, int output, size_t i, const char *name,
                 ONNXTensorElementDataType type, const int64_t *dims, size_t rank) {
  OrtTypeInfo *ti = NULL;
  const OrtTensorTypeAndShapeInfo *tensor = NULL;
  OrtAllocator *allocator = NULL;
  char *actual = NULL;
  int64_t d[3]; size_t r; ONNXTensorElementDataType t;
  TRY(v->api->GetAllocatorWithDefaultOptions(&allocator));
  TRY(output ? v->api->SessionGetOutputName(v->session,i,allocator,&actual) : v->api->SessionGetInputName(v->session,i,allocator,&actual));
  if (strcmp(actual,name)) goto mismatch;
  allocator->Free(allocator, actual); actual = NULL;
  TRY(output ? v->api->SessionGetOutputTypeInfo(v->session,i,&ti) : v->api->SessionGetInputTypeInfo(v->session,i,&ti));
  TRY(v->api->CastTypeInfoToTensorInfo(ti,&tensor));
  if (!tensor) goto mismatch;
  TRY(v->api->GetTensorElementType(tensor,&t));
  TRY(v->api->GetDimensionsCount(tensor,&r));
  if (t != type || r != rank || r > 3) goto mismatch;
  TRY(v->api->GetDimensions(tensor,d,r));
  for (size_t j=0;j<r;j++) if (d[j] != dims[j] && d[j] != -1) goto mismatch;
  v->api->ReleaseTypeInfo(ti); return 0;
mismatch:
  snprintf(v->error,sizeof(v->error),"incompatible Silero tensor %s",name); v->failed=1;
fail:
  if (actual && allocator) allocator->Free(allocator,actual);
  if (ti) v->api->ReleaseTypeInfo(ti);
  return -1;
}
int audsilero_available(void) {
#if defined(__arm__) && defined(__linux__)
  /* ORT 1.20.1 ARM MLAS build selects NEON; never execute it on older CPUs. */
  return (audplatform_auxv(AT_HWCAP) & HWCAP_NEON) != 0;
#else
  return 1;
#endif
}
AudSilero *audsilero_create(const char *path, char *error, size_t error_size) {
  AudSilero *v; OrtSessionOptions *opts=NULL; size_t ni,no;
  if(!path || !*path) { if(error && error_size) snprintf(error,error_size,"missing Silero model path"); return NULL; }
  if(!audsilero_available()) { if(error && error_size) snprintf(error,error_size,"Silero requires ARM NEON"); return NULL; }
  v=calloc(1,sizeof(*v));
  const int64_t in[]={1,576}, st[]={2,1,128}, out[]={1,1};
  if (!v) { if(error && error_size) snprintf(error,error_size,"out of memory"); return NULL; }
  v->library=dlopen("libonnxruntime.so.1",RTLD_NOW|RTLD_LOCAL);
  if(!v->library) { snprintf(v->error,sizeof(v->error),"Silero runtime load failed: %s",dlerror()); goto fail; }
  const OrtApiBase *(ORT_API_CALL *get_api_base)(void) =
    (const OrtApiBase *(ORT_API_CALL *)(void))dlsym(v->library,"OrtGetApiBase");
  if(!get_api_base) { snprintf(v->error,sizeof(v->error),"missing ONNX Runtime C API"); goto fail; }
  v->api=get_api_base()->GetApi(ORT_API_VERSION); v->rate=16000;
  if (!v->api) { snprintf(v->error,sizeof(v->error),"unsupported ONNX Runtime API"); goto fail; }
  TRY(v->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING,"nuvio-silero",&v->env));
  TRY(v->api->CreateSessionOptions(&opts));
  TRY(v->api->SetIntraOpNumThreads(opts,1)); TRY(v->api->SetInterOpNumThreads(opts,1));
  TRY(v->api->SetSessionExecutionMode(opts,ORT_SEQUENTIAL));
  TRY(v->api->AddSessionConfigEntry(opts,"session.intra_op.allow_spinning","0"));
  TRY(v->api->AddSessionConfigEntry(opts,"session.inter_op.allow_spinning","0"));
  TRY(v->api->SetSessionGraphOptimizationLevel(opts,ORT_DISABLE_ALL));
  TRY(v->api->CreateSession(v->env,path,opts,&v->session));
  v->api->ReleaseSessionOptions(opts); opts=NULL;
  TRY(v->api->SessionGetInputCount(v->session,&ni)); TRY(v->api->SessionGetOutputCount(v->session,&no));
  if (ni!=3 || no!=2) { snprintf(v->error,sizeof(v->error),"incompatible Silero input/output count"); goto fail; }
  if (shape(v,0,0,"input",ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,in,2) ||
      shape(v,0,1,"state",ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,st,3) ||
      shape(v,0,2,"sr",ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,NULL,0) ||
      shape(v,1,0,"output",ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,out,2) ||
      shape(v,1,1,"stateN",ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,st,3)) goto fail;
  TRY(v->api->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&v->memory));
  TRY(v->api->CreateTensorWithDataAsOrtValue(v->memory,v->input,sizeof(v->input),in,2,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&v->inputs[0]));
  TRY(v->api->CreateTensorWithDataAsOrtValue(v->memory,v->state,sizeof(v->state),st,3,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&v->inputs[1]));
  TRY(v->api->CreateTensorWithDataAsOrtValue(v->memory,&v->rate,sizeof(v->rate),NULL,0,ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,&v->inputs[2]));
  TRY(v->api->CreateTensorWithDataAsOrtValue(v->memory,&v->probability,sizeof(v->probability),out,2,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&v->outputs[0]));
  TRY(v->api->CreateTensorWithDataAsOrtValue(v->memory,v->output_state,sizeof(v->output_state),st,3,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&v->outputs[1]));
  return v;
fail:
  if(error && error_size) snprintf(error,error_size,"%s",v->error);
  if(opts) v->api->ReleaseSessionOptions(opts);
  audsilero_destroy(v); return NULL;
}
void audsilero_reset(AudSilero *v) {
  if(!v) return;
  memset(v->input,0,sizeof(v->input)); memset(v->state,0,sizeof(v->state));
  v->used=v->started=v->failed=v->count=v->active=v->flushed=0; v->samples=0; v->error[0]=0;
}
static int close_segment(AudSilero *v) {
  if(!v->active) return 0;
  v->active=0;
  if(v->end-v->begin < .2-1e-9) return 0;
  if(v->count==AUDVAD_MAX_SEG) { snprintf(v->error,sizeof(v->error),"speech segment capacity exceeded"); v->failed=1; return -1; }
  v->segments[v->count++]=(AudSeg){v->begin,v->end}; return 0;
}
static int frame(AudSilero *v,int n) {
  const char *ins[]={"input","state","sr"}, *outs[]={"output","stateN"};
  int64_t pts=v->origin+v->samples*1000000/16000;
  double start=pts/1e6,end=start+n/16000.;
  for(int i=0;i<512;i++) v->input[64+i]=i<n?v->pending[i]/32768.f:0;
  if(check(v,v->api->Run(v->session,NULL,ins,(const OrtValue *const *)v->inputs,3,outs,2,v->outputs))) return -1;
  if(!isfinite(v->probability) || v->probability<0 || v->probability>1) goto invalid;
  for(int i=0;i<256;i++) if(!isfinite(v->output_state[i])) goto invalid;
  memcpy(v->state,v->output_state,sizeof(v->state));
  memcpy(v->input,v->input+512,64*sizeof(float));
  if(v->observer) v->observer(v->opaque,pts,n,v->probability);
  if(v->probability >= .5f) {
    if(v->active && start-v->end>.300000001 && close_segment(v)) return -1;
    if(!v->active) { v->begin=start; v->active=1; }
    v->end=end;
  } else if(v->active && end-v->end>.300000001 && close_segment(v)) return -1;
  v->samples+=n; v->used=0; return 0;
invalid:
  snprintf(v->error,sizeof(v->error),"nonfinite or invalid Silero output"); v->failed=1; return -1;
}
int audsilero_feed(AudSilero *v,const int16_t *pcm,int n,int64_t pts) {
  if(!v || n<0 || (n && !pcm) || v->failed || v->flushed) return -1;
  if(!n) return 0;
  if(v->started) {
    int64_t expected=v->origin+(v->samples+v->used)*1000000/16000;
    if(llabs(pts-expected)>125) audsilero_reset(v);
  }
  if(!v->started) { v->origin=pts; v->started=1; }
  while(n) { int take=512-v->used; if(take>n) take=n;
    memcpy(v->pending+v->used,pcm,take*sizeof(*pcm)); v->used+=take; pcm+=take; n-=take;
    if(v->used==512 && frame(v,512)) return -1;
  } return 0;
}
int audsilero_flush(AudSilero *v) {
  if(!v || v->failed) return -1;
  if(v->flushed) return 0;
  if(v->used && frame(v,v->used)) return -1;
  v->flushed=1; return close_segment(v);
}
const AudSeg *audsilero_segments(const AudSilero *v,int *count) { if(count) *count=v?v->count:0; return v?v->segments:NULL; }
const char *audsilero_error(const AudSilero *v) { return v?v->error:"Silero unavailable"; }
void audsilero_observe(AudSilero *v,AudSileroProbability cb,void *opaque) { if(v) { v->observer=cb; v->opaque=opaque; } }
void audsilero_destroy(AudSilero *v) {
  if(!v) return;
  if(v->api) { for(int i=0;i<3;i++) if(v->inputs[i]) v->api->ReleaseValue(v->inputs[i]);
    for(int i=0;i<2;i++) if(v->outputs[i]) v->api->ReleaseValue(v->outputs[i]);
    if(v->memory) v->api->ReleaseMemoryInfo(v->memory);
    if(v->session) v->api->ReleaseSession(v->session);
    if(v->env) v->api->ReleaseEnv(v->env);
  } if(v->library) dlclose(v->library); free(v);
}
#else
int audsilero_available(void) { return 0; }
AudSilero *audsilero_create(const char *path,char *error,size_t n) { (void)path; if(error && n) snprintf(error,n,"Silero runtime not built for this platform"); return NULL; }
void audsilero_reset(AudSilero *v) { (void)v; }
int audsilero_feed(AudSilero *v,const int16_t *p,int n,int64_t t) { (void)v;(void)p;(void)n;(void)t; return -1; }
int audsilero_flush(AudSilero *v) { (void)v;return -1; }
const AudSeg *audsilero_segments(const AudSilero *v,int *n) { (void)v;if(n)*n=0;return NULL; }
const char *audsilero_error(const AudSilero *v) { (void)v;return "Silero runtime not built for this platform"; }
void audsilero_observe(AudSilero *v,AudSileroProbability cb,void *o) { (void)v;(void)cb;(void)o; }
void audsilero_destroy(AudSilero *v) { (void)v; }
#endif
int audsilero_validate_model(const char *path,char *error,size_t n) {
  AudSilero *v=audsilero_create(path,error,n); if(!v) return -1;
  int16_t silence[512]={0}; int result=audsilero_feed(v,silence,512,0);
  if(result && error && n) snprintf(error,n,"%s",audsilero_error(v));
  audsilero_destroy(v); return result;
}
