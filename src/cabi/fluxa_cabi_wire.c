#include "fluxa_cabi_wire.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifdef FLUXA_CABI_SODIUM
#include <sodium.h>
#endif

#define FXCB_HEADER 12u
#define FXCB_ITEM_HEADER 8u
#define FXCS_HEADER 32u

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(unsigned char *p, uint32_t v) {
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static void wr64(unsigned char *p, uint64_t v) {
    wr32(p,(uint32_t)v); wr32(p+4,(uint32_t)(v>>32));
}
static uint64_t rd64(const unsigned char *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p+4)<<32);
}

static int reserve(fluxa_cabi_message *m, uint32_t add) {
    uint64_t need64;
    uint32_t need, cap;
    unsigned char *p;
    if (!m) return 0;
    need64=(uint64_t)m->size+(uint64_t)add;
    if (need64>UINT32_MAX) return 0;
    need=(uint32_t)need64;
    if (need<=m->capacity) return 1;
    cap=m->capacity?m->capacity:256u;
    while(cap<need) {
        uint32_t next=cap<=UINT32_MAX/2u?cap*2u:need;
        if(next<=cap){cap=need;break;}
        cap=next;
    }
    p=(unsigned char*)realloc(m->data,cap);
    if(!p) return 0;
    m->data=p; m->capacity=cap; return 1;
}
static int ensure_header(fluxa_cabi_message *m) {
    unsigned char *p;
    if (!m) return 0;
    if (m->size) return m->size>=FXCB_HEADER;
    if (!reserve(m,FXCB_HEADER)) return 0;
    p=(unsigned char*)m->data;
    memcpy(p,"FXCB",4); p[4]=(unsigned char)FLUXA_CABI_WIRE_VERSION;
    p[5]=p[6]=p[7]=0; wr32(p+8,0); m->size=FXCB_HEADER; return 1;
}

void fluxa_cabi_message_init(fluxa_cabi_message *m) { if(m) memset(m,0,sizeof(*m)); }
void fluxa_cabi_message_reset(fluxa_cabi_message *m) { if(m) m->size=0; }
void fluxa_cabi_message_free(fluxa_cabi_message *m) {
    if (!m) return;
    free(m->data);
    memset(m, 0, sizeof(*m));
}

int fxcabi_wire_add_tagged_raw(fluxa_cabi_message *m, uint8_t tag,
                               const void *payload, uint32_t payload_size) {
    unsigned char *p; uint32_t count;
    if(payload_size && !payload) return 0;
    if(!ensure_header(m)) return 0;
    if(!reserve(m,FXCB_ITEM_HEADER+payload_size)) return 0;
    p=(unsigned char*)m->data+m->size;
    p[0]=tag; p[1]=p[2]=p[3]=0; wr32(p+4,payload_size);
    if(payload_size) memcpy(p+8,payload,payload_size);
    m->size += FXCB_ITEM_HEADER+payload_size;
    count=rd32((unsigned char*)m->data+8);
    if(count==UINT32_MAX) return 0;
    wr32((unsigned char*)m->data+8,count+1u);
    return 1;
}

int fluxa_cabi_add_int(fluxa_cabi_message *m,int32_t v){unsigned char b[4];wr32(b,(uint32_t)v);return fxcabi_wire_add_tagged_raw(m,FLUXA_CABI_INT,b,4);}
int fluxa_cabi_add_float(fluxa_cabi_message *m,double v){uint64_t u;unsigned char b[8];memcpy(&u,&v,8);wr64(b,u);return fxcabi_wire_add_tagged_raw(m,FLUXA_CABI_FLOAT,b,8);}
int fluxa_cabi_add_bool(fluxa_cabi_message *m,int v){unsigned char b=(unsigned char)(v?1:0);return fxcabi_wire_add_tagged_raw(m,FLUXA_CABI_BOOL,&b,1);}
int fluxa_cabi_add_str(fluxa_cabi_message *m,const char *s,uint32_t n){return fxcabi_wire_add_tagged_raw(m,FLUXA_CABI_STR,s,n);}

static int add_arr_fixed(fluxa_cabi_message*m,uint8_t tag,const void*vals,uint32_t count,uint32_t width,int kind){
    fluxa_cabi_message tmp={0}; unsigned char *p; uint64_t n64=4ull+(uint64_t)count*width; uint32_t i;
    if (count && !vals) return 0;
    if (n64 > UINT32_MAX) return 0;
    if (!reserve(&tmp, (uint32_t)n64)) return 0;
    tmp.size = (uint32_t)n64;
    p = (unsigned char *)tmp.data;
    wr32(p, count);
    if(kind==1){const int32_t*a=(const int32_t*)vals;for(i=0;i<count;i++)wr32(p+4+i*4,(uint32_t)a[i]);}
    else if(kind==2){const double*a=(const double*)vals;for(i=0;i<count;i++){uint64_t u;memcpy(&u,&a[i],8);wr64(p+4+i*8,u);}}
    else {const uint8_t*a=(const uint8_t*)vals;for(i=0;i<count;i++)p[4+i]=(unsigned char)(a[i]?1:0);}
    i=fxcabi_wire_add_tagged_raw(m,tag,tmp.data,tmp.size); fluxa_cabi_message_free(&tmp); return (int)i;
}
int fluxa_cabi_add_int_arr(fluxa_cabi_message*m,const int32_t*v,uint32_t n){return add_arr_fixed(m,FLUXA_CABI_ARR_INT,v,n,4,1);}
int fluxa_cabi_add_float_arr(fluxa_cabi_message*m,const double*v,uint32_t n){return add_arr_fixed(m,FLUXA_CABI_ARR_FLOAT,v,n,8,2);}
int fluxa_cabi_add_bool_arr(fluxa_cabi_message*m,const uint8_t*v,uint32_t n){return add_arr_fixed(m,FLUXA_CABI_ARR_BOOL,v,n,1,3);}
int fluxa_cabi_add_str_arr(fluxa_cabi_message*m,const fluxa_cabi_str_view*v,uint32_t n){
    fluxa_cabi_message t={0};uint32_t i;unsigned char b[4];if(n&&!v)return 0;if(!reserve(&t,4))return 0;wr32((unsigned char*)t.data,n);t.size=4;
    for(i=0;i<n;i++){if(v[i].size&&!v[i].data){fluxa_cabi_message_free(&t);return 0;}wr32(b,v[i].size);if(!reserve(&t,4+v[i].size)){fluxa_cabi_message_free(&t);return 0;}memcpy((unsigned char*)t.data+t.size,b,4);t.size+=4;if(v[i].size){memcpy((unsigned char*)t.data+t.size,v[i].data,v[i].size);t.size+=v[i].size;}}
    i=fxcabi_wire_add_tagged_raw(m,FLUXA_CABI_ARR_STR,t.data,t.size);fluxa_cabi_message_free(&t);return (int)i;
}

static int payload_valid(uint8_t tag,const unsigned char*p,uint32_t n){
    uint32_t c,i,off;
    switch(tag){
    case FLUXA_CABI_INT:return n==4;
    case FLUXA_CABI_FLOAT:return n==8;
    case FLUXA_CABI_BOOL:return n==1 && p[0]<=1;
    case FLUXA_CABI_STR:return 1;
    case FLUXA_CABI_ARR_INT: if(n<4)return 0;c=rd32(p);return (uint64_t)4+c*4ull==n;
    case FLUXA_CABI_ARR_FLOAT: if(n<4)return 0;c=rd32(p);return 4ull+c*8ull==n;
    case FLUXA_CABI_ARR_BOOL: if(n<4)return 0;c=rd32(p);if(4ull+c!=n)return 0;for(i=0;i<c;i++)if(p[4+i]>1)return 0;return 1;
    case FLUXA_CABI_ARR_STR: if(n<4)return 0;c=rd32(p);off=4;for(i=0;i<c;i++){uint32_t l;if(off>n||n-off<4)return 0;l=rd32(p+off);off+=4;if(l>n-off)return 0;off+=l;}return off==n;
    default:return 0;
    }
}
int fluxa_cabi_view_validate(const fluxa_cabi_view*v){
    const unsigned char*p;uint32_t count,i,off;
    if (!v || !v->data || v->size < FXCB_HEADER) return 0;
    p = (const unsigned char *)v->data;
    if(memcmp(p,"FXCB",4)||p[4]!=FLUXA_CABI_WIRE_VERSION||p[5]||p[6]||p[7])return 0;
    count=rd32(p+8);off=FXCB_HEADER;
    for(i=0;i<count;i++){uint32_t n;if(off>v->size||v->size-off<FXCB_ITEM_HEADER)return 0;n=rd32(p+off+4);if(n>v->size-off-FXCB_ITEM_HEADER)return 0;if(!payload_valid(p[off],p+off+8,n))return 0;off+=FXCB_ITEM_HEADER+n;}
    return off==v->size;
}
uint32_t fluxa_cabi_value_count(const fluxa_cabi_view*v){if(!fluxa_cabi_view_validate(v))return 0;return rd32((const unsigned char*)v->data+8);}
int fxcabi_wire_locate(const fluxa_cabi_view*v,uint32_t idx,fluxa_cabi_type*t,const unsigned char**payload,uint32_t*n,uint32_t*ac){
    const unsigned char*p;uint32_t count,i,off;if(!fluxa_cabi_view_validate(v))return 0;p=(const unsigned char*)v->data;count=rd32(p+8);if(idx>=count)return 0;off=FXCB_HEADER;
    for(i=0;i<=idx;i++){uint32_t z=rd32(p+off+4);if(i==idx){if(t)*t=(fluxa_cabi_type)p[off];if(payload)*payload=p+off+8;if(n)*n=z;if(ac)*ac=(p[off]>=FLUXA_CABI_ARR_INT&&p[off]<=FLUXA_CABI_ARR_STR)?rd32(p+off+8):0;return 1;}off+=FXCB_ITEM_HEADER+z;}return 0;
}
int fluxa_cabi_value_type(const fluxa_cabi_view*v,uint32_t i,fluxa_cabi_type*out){return out&&fxcabi_wire_locate(v,i,out,NULL,NULL,NULL);}
int fluxa_cabi_get_int(const fluxa_cabi_view*v,uint32_t i,int32_t*out){fluxa_cabi_type t;const unsigned char*p;return out&&fxcabi_wire_locate(v,i,&t,&p,NULL,NULL)&&t==FLUXA_CABI_INT?(*out=(int32_t)rd32(p),1):0;}
int fluxa_cabi_get_float(const fluxa_cabi_view*v,uint32_t i,double*out){fluxa_cabi_type t;const unsigned char*p;uint64_t u;if(!out||!fxcabi_wire_locate(v,i,&t,&p,NULL,NULL)||t!=FLUXA_CABI_FLOAT)return 0;u=rd64(p);memcpy(out,&u,8);return 1;}
int fluxa_cabi_get_bool(const fluxa_cabi_view*v,uint32_t i,int*out){fluxa_cabi_type t;const unsigned char*p;return out&&fxcabi_wire_locate(v,i,&t,&p,NULL,NULL)&&t==FLUXA_CABI_BOOL?(*out=p[0]?1:0,1):0;}
int fluxa_cabi_get_str(const fluxa_cabi_view*v,uint32_t i,fluxa_cabi_str_view*out){fluxa_cabi_type t;const unsigned char*p;uint32_t n;if(!out||!fxcabi_wire_locate(v,i,&t,&p,&n,NULL)||t!=FLUXA_CABI_STR)return 0;out->data=(const char*)p;out->size=n;return 1;}
int fluxa_cabi_get_arr_count(const fluxa_cabi_view*v,uint32_t i,uint32_t*out){fluxa_cabi_type t;uint32_t c;if(!out||!fxcabi_wire_locate(v,i,&t,NULL,NULL,&c)||t<FLUXA_CABI_ARR_INT||t>FLUXA_CABI_ARR_STR)return 0;*out=c;return 1;}
int fluxa_cabi_get_int_arr_value(const fluxa_cabi_view*v,uint32_t i,uint32_t e,int32_t*out){fluxa_cabi_type t;const unsigned char*p;uint32_t c;if(!out||!fxcabi_wire_locate(v,i,&t,&p,NULL,&c)||t!=FLUXA_CABI_ARR_INT||e>=c)return 0;*out=(int32_t)rd32(p+4+e*4);return 1;}
int fluxa_cabi_get_float_arr_value(const fluxa_cabi_view*v,uint32_t i,uint32_t e,double*out){fluxa_cabi_type t;const unsigned char*p;uint32_t c;uint64_t u;if(!out||!fxcabi_wire_locate(v,i,&t,&p,NULL,&c)||t!=FLUXA_CABI_ARR_FLOAT||e>=c)return 0;u=rd64(p+4+e*8);memcpy(out,&u,8);return 1;}
int fluxa_cabi_get_bool_arr_value(const fluxa_cabi_view*v,uint32_t i,uint32_t e,int*out){fluxa_cabi_type t;const unsigned char*p;uint32_t c;if(!out||!fxcabi_wire_locate(v,i,&t,&p,NULL,&c)||t!=FLUXA_CABI_ARR_BOOL||e>=c)return 0;*out=p[4+e]?1:0;return 1;}
int fluxa_cabi_get_str_arr_value(const fluxa_cabi_view*v,uint32_t i,uint32_t e,fluxa_cabi_str_view*out){fluxa_cabi_type t;const unsigned char*p;uint32_t c,k,off=4;if(!out||!fxcabi_wire_locate(v,i,&t,&p,NULL,&c)||t!=FLUXA_CABI_ARR_STR||e>=c)return 0;for(k=0;k<c;k++){uint32_t n=rd32(p+off);off+=4;if(k==e){out->data=(const char*)(p+off);out->size=n;return 1;}off+=n;}return 0;}

int fluxa_cabi_security_available(void){
#ifdef FLUXA_CABI_SODIUM
    return sodium_init()>=0;
#else
    return 0;
#endif
}
static int secfail(fluxa_cabi_error*e,const char*m){if(e){memset(e,0,sizeof(*e));e->code=FLUXA_CABI_ESECURITY;strncpy(e->context,"cabi.security",sizeof(e->context)-1);strncpy(e->message,m,sizeof(e->message)-1);}return FLUXA_CABI_ESECURITY;}
int fluxa_cabi_seal(const uint8_t key[FLUXA_CABI_KEY_BYTES],const fluxa_cabi_view*clear,fluxa_cabi_message*sealed,fluxa_cabi_error*e){
#ifdef FLUXA_CABI_SODIUM
    unsigned char *p;unsigned long long clen;if(!key||!clear||!sealed||!fluxa_cabi_view_validate(clear))return secfail(e,"invalid key or clear FXCB frame");if(sodium_init()<0)return secfail(e,"libsodium initialization failed");fluxa_cabi_message_reset(sealed);if(!reserve(sealed,FXCS_HEADER+clear->size+crypto_aead_xchacha20poly1305_ietf_ABYTES))return secfail(e,"out of memory");p=(unsigned char*)sealed->data;memcpy(p,"FXCS",4);p[4]=1;p[5]=p[6]=p[7]=0;randombytes_buf(p+8,24);if(crypto_aead_xchacha20poly1305_ietf_encrypt(p+FXCS_HEADER,&clen,(const unsigned char*)clear->data,clear->size,(const unsigned char*)"FXCB1",5,NULL,p+8,key)!=0)return secfail(e,"encryption failed");sealed->size=FXCS_HEADER+(uint32_t)clen;return FLUXA_CABI_OK;
#else
    (void)key;(void)clear;(void)sealed;return secfail(e,"secure C ABI requires std.crypto/libsodium at build time");
#endif
}
int fluxa_cabi_unseal(const uint8_t key[FLUXA_CABI_KEY_BYTES],const fluxa_cabi_view*sealed,fluxa_cabi_message*clear,fluxa_cabi_error*e){
#ifdef FLUXA_CABI_SODIUM
    const unsigned char*p;unsigned char*out;unsigned long long n;if(!key||!sealed||!clear||!sealed->data||sealed->size<FXCS_HEADER+crypto_aead_xchacha20poly1305_ietf_ABYTES)return secfail(e,"invalid sealed frame");p=(const unsigned char*)sealed->data;if(memcmp(p,"FXCS",4)||p[4]!=1)return secfail(e,"unsupported secure envelope");if(sodium_init()<0)return secfail(e,"libsodium initialization failed");fluxa_cabi_message_reset(clear);if(!reserve(clear,sealed->size))return secfail(e,"out of memory");out=(unsigned char*)clear->data;if(crypto_aead_xchacha20poly1305_ietf_decrypt(out,&n,NULL,p+FXCS_HEADER,sealed->size-FXCS_HEADER,(const unsigned char*)"FXCB1",5,p+8,key)!=0)return secfail(e,"authentication/decryption failed");if(n>UINT32_MAX)return secfail(e,"clear frame too large");clear->size=(uint32_t)n;{fluxa_cabi_view v={clear->data,clear->size};if(!fluxa_cabi_view_validate(&v)){clear->size=0;return secfail(e,"decrypted bytes are not a valid FXCB frame");}}return FLUXA_CABI_OK;
#else
    (void)key;(void)sealed;(void)clear;return secfail(e,"secure C ABI requires std.crypto/libsodium at build time");
#endif
}
