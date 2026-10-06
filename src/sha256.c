#include "sha256.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint32_t h[8]; uint64_t n; unsigned char b[64]; size_t k; } At_Sha;
static void at_sha_bloco(At_Sha *s, const unsigned char *p) {
  static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
  uint32_t w[64], a,b,c,d,e,f,g,h; int i;
  for (i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
  for (i = 16; i < 64; i++) {
    uint32_t s0 = (w[i-15]>>7|w[i-15]<<25) ^ (w[i-15]>>18|w[i-15]<<14) ^ (w[i-15]>>3);
    uint32_t s1 = (w[i-2]>>17|w[i-2]<<15) ^ (w[i-2]>>19|w[i-2]<<13) ^ (w[i-2]>>10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
  for (i = 0; i < 64; i++) {
    uint32_t S1 = (e>>6|e<<26) ^ (e>>11|e<<21) ^ (e>>25|e<<7);
    uint32_t ch = (e&f) ^ (~e&g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = (a>>2|a<<30) ^ (a>>13|a<<19) ^ (a>>22|a<<10);
    uint32_t maj = (a&b) ^ (a&c) ^ (b&c);
    uint32_t t2 = S0 + maj;
    h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
  }
  s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void at_sha_init(At_Sha *s) {
  s->h[0]=0x6a09e667;s->h[1]=0xbb67ae85;s->h[2]=0x3c6ef372;s->h[3]=0xa54ff53a;
  s->h[4]=0x510e527f;s->h[5]=0x9b05688c;s->h[6]=0x1f83d9ab;s->h[7]=0x5be0cd19;
  s->n=0;s->k=0;
}
static void at_sha_up(At_Sha *s, const unsigned char *p, size_t n) {
  s->n += (uint64_t)n * 8;
  while (n) {
    size_t take = 64 - s->k; if (take > n) take = n;
    memcpy(s->b + s->k, p, take); s->k += take; p += take; n -= take;
    if (s->k == 64) { at_sha_bloco(s, s->b); s->k = 0; }
  }
}
static void at_sha_fim(At_Sha *s, char *hex65) {
  unsigned char len[8]; int i;
  uint64_t bits = s->n;
  unsigned char um = 0x80;
  for (i = 0; i < 8; i++) len[7-i] = (unsigned char)(bits >> (i*8));
  at_sha_up(s, &um, 1);
  while (s->k != 56) { unsigned char z = 0; at_sha_up(s, &z, 1); }
  at_sha_up(s, len, 8);
  for (i = 0; i < 8; i++)
    snprintf(hex65 + i*8, 9, "%08x", s->h[i]);
}
// sha256 hex de um buffer inteiro em memoria -> hex65 (64 chars + NUL).
void nv_sha256_hex(const unsigned char *buf, size_t n, char *hex65) {
  At_Sha s; at_sha_init(&s); at_sha_up(&s, buf, n); at_sha_fim(&s, hex65);
}
