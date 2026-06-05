/* vknn.h — approximate KD-tree 5-NN for the Rinha 2026 fraud-detection vectors.
 * INT16 storage (value x10000, exact for the 4-decimal references) + integer
 * squared-distance (int64, exact). 14-dim, 3,000,000 refs:
 *   - storage ~84MB (2 bytes/dim) -> two symmetric API instances fit 350MB.
 *   - distances are exact for 4-decimal refs -> matches exact 5-NN (the #1 uses
 *     the same int16 x10000 trick; "exact because 14*10000^2 < INT32_MAX").
 *   - EXACT search (budget<=0, geometric prune only): ~0.2 ms/query, 100% match.
 * Shared by build_index.c (offline) and std.libv (runtime, mmap RO + warm). */
#ifndef VKNN_H
#define VKNN_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define VKNN_DIM    14
#define VKNN_LEAF   24
#define VKNN_SCALE  10000
#define VKNN_MAGIC  0x564B4E32u   /* "VKN2" (int16) */

typedef struct { int32_t dim; int32_t val; int32_t left, right, lo, hi; } VkNode; /* val = split, x10000 */
typedef struct {
    int32_t n, dim, nnodes;
    const int16_t *pts;   /* [n*dim] mmap'd, KD-reordered, x10000 */
    const uint8_t *lab;   /* [n]     mmap'd                       */
    const VkNode  *nodes; /* [nnodes]mmap'd                       */
    void  *map; size_t maplen;
} VkIndex;
typedef struct { uint32_t magic; int32_t n, dim, nnodes; } VkHeader;

static inline int16_t vk__q(double x){ /* quantize to int16 x10000, rounded */
    double v = x * (double)VKNN_SCALE;
    long r = (long)(v < 0 ? v - 0.5 : v + 0.5);
    if (r >  32767) r =  32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
}

/* ---------- build (offline, on float input; quantizes on write) ---------- */
typedef struct { const float *pts; const uint8_t *lab; int *idx; VkNode *nodes; int nnodes, cap; } VkBuild;
static inline int vk__newnode(VkBuild *b){ if(b->nnodes==b->cap){b->cap*=2;b->nodes=(VkNode*)realloc(b->nodes,(size_t)b->cap*sizeof(VkNode));} return b->nnodes++; }
static inline int vk__build(VkBuild *b, int lo, int hi){
    int id=vk__newnode(b);
    b->nodes[id].lo=lo; b->nodes[id].hi=hi; b->nodes[id].left=b->nodes[id].right=-1;
    if(hi-lo<=VKNN_LEAF){ b->nodes[id].dim=-1; b->nodes[id].val=0; return id; }
    double mean[VKNN_DIM]={0}, var[VKNN_DIM]={0}; int n=hi-lo;
    for(int i=lo;i<hi;i++){ const float*p=b->pts+(size_t)b->idx[i]*VKNN_DIM; for(int d=0;d<VKNN_DIM;d++) mean[d]+=p[d]; }
    for(int d=0;d<VKNN_DIM;d++) mean[d]/=n;
    for(int i=lo;i<hi;i++){ const float*p=b->pts+(size_t)b->idx[i]*VKNN_DIM; for(int d=0;d<VKNN_DIM;d++){double df=p[d]-mean[d];var[d]+=df*df;} }
    int bd=0; for(int d=1;d<VKNN_DIM;d++) if(var[d]>var[bd]) bd=d;
    int mid=(lo+hi)/2, l=lo, r=hi-1;
    while(l<r){
        float pivot=b->pts[(size_t)b->idx[(l+r)/2]*VKNN_DIM+bd]; int i=l,j=r;
        while(i<=j){
            while(b->pts[(size_t)b->idx[i]*VKNN_DIM+bd]<pivot) i++;
            while(b->pts[(size_t)b->idx[j]*VKNN_DIM+bd]>pivot) j--;
            if(i<=j){int t=b->idx[i];b->idx[i]=b->idx[j];b->idx[j]=t;i++;j--;}
        }
        if(mid<=j) r=j; else if(mid>=i) l=i; else break;
    }
    b->nodes[id].dim=bd; b->nodes[id].val=vk__q(b->pts[(size_t)b->idx[mid]*VKNN_DIM+bd]);
    int L=vk__build(b,lo,mid), R=vk__build(b,mid,hi);
    b->nodes[id].left=L; b->nodes[id].right=R;
    return id;
}
static inline int vk_build_and_save(const float *pts0, const uint8_t *lab0, int n, const char *out_path){
    VkBuild b; b.cap=1024; b.nnodes=0; b.pts=pts0; b.lab=lab0;
    b.idx=(int*)malloc((size_t)n*sizeof(int)); for(int i=0;i<n;i++) b.idx[i]=i;
    b.nodes=(VkNode*)malloc((size_t)b.cap*sizeof(VkNode));
    vk__build(&b,0,n);
    FILE*o=fopen(out_path,"wb"); if(!o) return -1;
    VkHeader h={VKNN_MAGIC,n,VKNN_DIM,b.nnodes}; fwrite(&h,sizeof(h),1,o);
    int16_t row[VKNN_DIM];
    for(int i=0;i<n;i++){ const float*p=pts0+(size_t)b.idx[i]*VKNN_DIM;
        for(int d=0;d<VKNN_DIM;d++) row[d]=vk__q(p[d]);
        fwrite(row,sizeof(int16_t),VKNN_DIM,o);
    }
    for(int i=0;i<n;i++) fwrite(&lab0[b.idx[i]],1,1,o);
    fwrite(b.nodes,sizeof(VkNode),b.nnodes,o);
    fclose(o); free(b.idx); free(b.nodes);
    return b.nnodes;
}

/* ---------- load (runtime, mmap RO) ---------- */
static inline int vk_load(VkIndex *ix, const char *path){
    memset(ix,0,sizeof(*ix));
    int fd=open(path,O_RDONLY); if(fd<0) return -1;
    struct stat st; if(fstat(fd,&st)!=0){close(fd);return -2;}
    void*m=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(m==MAP_FAILED) return -3;
    const VkHeader*h=(const VkHeader*)m;
    if(h->magic!=VKNN_MAGIC||h->dim!=VKNN_DIM){munmap(m,st.st_size);return -4;}
    ix->map=m; ix->maplen=st.st_size; ix->n=h->n; ix->dim=h->dim; ix->nnodes=h->nnodes;
    const char*p=(const char*)m+sizeof(VkHeader);
    ix->pts=(const int16_t*)p;          p+=(size_t)h->n*VKNN_DIM*sizeof(int16_t);
    ix->lab=(const uint8_t*)p;          p+=(size_t)h->n;
    ix->nodes=(const VkNode*)p;
#ifdef MADV_WILLNEED
    madvise(m,st.st_size,MADV_WILLNEED);
#endif
    return 0;
}

/* Pre-touch every page so the mmap'd index is RAM-resident before serving.
 * Call once at startup; only return 200 on /ready AFTER this (avoids a cold-page
 * latency spike on the first requests — mirrors the #1's keep-warm). */
static inline void vk_warm(VkIndex *ix){
    if(!ix->map) return;
    const volatile uint8_t *p=(const uint8_t*)ix->map; volatile uint64_t s=0;
    for(size_t off=0; off<ix->maplen; off+=4096) s+=p[off];
    (void)s;
}

/* ---------- score (runtime, integer distance) ---------- */
static inline void vk__ins(int64_t d, uint8_t lab, int k, int64_t *bd, uint8_t *bl){
    if(d>=bd[k-1]) return;
    int pos=k-1;
    while(pos>0 && bd[pos-1]>d){bd[pos]=bd[pos-1];bl[pos]=bl[pos-1];pos--;}
    bd[pos]=d; bl[pos]=lab;
}
static inline void vk__search(const VkIndex *ix, int node, const int16_t *q, int k, int64_t *bd, uint8_t *bl, int *budget){
    const VkNode *nd=&ix->nodes[node];
    if(nd->dim<0){
        for(int i=nd->lo;i<nd->hi;i++){
            const int16_t*p=ix->pts+(size_t)i*VKNN_DIM; int64_t d=0;
            for(int j=0;j<VKNN_DIM;j++){ int diff=(int)q[j]-(int)p[j]; d+=(int64_t)diff*diff; }
            vk__ins(d,ix->lab[i],k,bd,bl);
        }
        (*budget)--; return;
    }
    int diff=(int)q[nd->dim]-nd->val;
    int near=diff<0?nd->left:nd->right, far=diff<0?nd->right:nd->left;
    vk__search(ix,near,q,k,bd,bl,budget);
    if(*budget>0 && (int64_t)diff*diff<bd[k-1]) vk__search(ix,far,q,k,bd,bl,budget);
}
/* Returns the number of frauds among the k nearest (0..k). budget<=0 => EXACT. */
static inline int vk_count(const VkIndex *ix, const float *qf, int k, int budget){
    int16_t q[VKNN_DIM]; for(int j=0;j<VKNN_DIM;j++) q[j]=vk__q(qf[j]);
    int64_t bd[8]; uint8_t bl[8]; if(k>8)k=8;
    for(int i=0;i<8;i++){bd[i]=(int64_t)1<<62;bl[i]=0;}
    int leaves=(budget<=0)?0x7fffffff:budget;
    vk__search(ix,0,q,k,bd,bl,&leaves);
    int fr=0; for(int i=0;i<k;i++) fr+=bl[i];
    return fr;
}
static inline float vk_score(const VkIndex *ix, const float *qf, int k, int budget){
    return (float)vk_count(ix,qf,k,budget) / (float)(k<=8?k:8);
}
static inline void vk_free(VkIndex *ix){ if(ix->map) munmap(ix->map,ix->maplen); ix->map=NULL; }
#endif
