/* vknn.h — KD-tree 5-NN for the Rinha 2026 fraud-detection vectors. v3.
 * INT16 storage (value x10000, exact for the 4-decimal references) + integer
 * squared-distance (int64, exact). 14-dim, 3,000,000 refs.
 *
 * v3 (VKN3): every node carries a 14-d AABB (int16 min/max per dim) and the
 * search is iterative BEST-FIRST with box-distance pruning. A split-plane
 * bound uses ONE dimension and is nearly useless in 14-d (off-manifold exact
 * queries degenerated to scanning ~60% of the tree, ~23 ms); the full box
 * lower bound prunes across ALL dims and visits children nearest-box-first,
 * which keeps EXACT search fast in both regimes. budget (leaf-visit cap)
 * still applies on top as an optional circuit breaker (<=0 = exact).
 *
 *   - storage: pts 84MB + labels 3MB + nodes ~21MB (80B/node) ≈ 108MB.
 *   - distances are exact for 4-decimal refs (14*10000^2 < INT32_MAX trick).
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
#define VKNN_MAGIC  0x564B4E33u   /* "VKN3" (int16 + per-node AABB) */
#define VKNN_STACK  128           /* best-first stack capacity (depth ~18)  */

typedef struct {
    int32_t dim;                  /* split dim, -1 for leaf                 */
    int32_t val;                  /* split value, x10000                    */
    int32_t left, right, lo, hi;
    int16_t mn[VKNN_DIM];         /* AABB of all points under this node     */
    int16_t mx[VKNN_DIM];
} VkNode;                         /* 24 + 56 = 80 bytes                     */

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
/* compute per-node AABBs (quantized space) bottom-up after the build */
static inline void vk__boxes(VkBuild *b, int id){
    VkNode *nd=&b->nodes[id];
    if(nd->dim<0){
        for(int d=0;d<VKNN_DIM;d++){ nd->mn[d]=32767; nd->mx[d]=-32768; }
        for(int i=nd->lo;i<nd->hi;i++){
            const float *p=b->pts+(size_t)b->idx[i]*VKNN_DIM;
            for(int d=0;d<VKNN_DIM;d++){
                int16_t v=vk__q(p[d]);
                if(v<nd->mn[d]) nd->mn[d]=v;
                if(v>nd->mx[d]) nd->mx[d]=v;
            }
        }
        return;
    }
    vk__boxes(b,nd->left); vk__boxes(b,nd->right);
    const VkNode *L=&b->nodes[nd->left], *R=&b->nodes[nd->right];
    for(int d=0;d<VKNN_DIM;d++){
        nd->mn[d] = (L->mn[d]<R->mn[d]) ? L->mn[d] : R->mn[d];
        nd->mx[d] = (L->mx[d]>R->mx[d]) ? L->mx[d] : R->mx[d];
    }
}
static inline int vk_build_and_save(const float *pts0, const uint8_t *lab0, int n, const char *out_path){
    VkBuild b; b.cap=1024; b.nnodes=0; b.pts=pts0; b.lab=lab0;
    b.idx=(int*)malloc((size_t)n*sizeof(int)); for(int i=0;i<n;i++) b.idx[i]=i;
    b.nodes=(VkNode*)malloc((size_t)b.cap*sizeof(VkNode));
    vk__build(&b,0,n);
    vk__boxes(&b,0);
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
 * latency spike on the first requests). */
static inline void vk_warm(VkIndex *ix){
    if(!ix->map) return;
    const volatile uint8_t *p=(const uint8_t*)ix->map; volatile uint64_t s=0;
    for(size_t off=0; off<ix->maplen; off+=4096) s+=p[off];
    (void)s;
}

/* ---------- search (runtime, integer distance, best-first + box prune) ---------- */
static inline void vk__ins(int64_t d, uint8_t lab, int k, int64_t *bd, uint8_t *bl){
    if(d>=bd[k-1]) return;
    int pos=k-1;
    while(pos>0 && bd[pos-1]>d){bd[pos]=bd[pos-1];bl[pos]=bl[pos-1];pos--;}
    bd[pos]=d; bl[pos]=lab;
}
/* squared distance from query to the node's AABB (0 if inside) */
static inline int64_t vk__box_lb(const VkNode *nd, const int16_t *q){
    int64_t s=0;
    for(int j=0;j<VKNN_DIM;j++){
        int v=q[j], d=0;
        if(v<nd->mn[j]) d=nd->mn[j]-v;
        else if(v>nd->mx[j]) d=v-nd->mx[j];
        s+=(int64_t)d*(int64_t)d;
    }
    return s;
}
static inline void vk__leaf_scan(const VkIndex *ix, const VkNode *nd, const int16_t *q,
                                 int k, int64_t *bd, uint8_t *bl){
    for(int i=nd->lo;i<nd->hi;i++){
        const int16_t*p=ix->pts+(size_t)i*VKNN_DIM; int64_t d=0;
        for(int j=0;j<VKNN_DIM;j++){ int diff=(int)q[j]-(int)p[j]; d+=(int64_t)diff*diff; }
        vk__ins(d,ix->lab[i],k,bd,bl);
    }
}
/* Best-first exact/bounded search. Returns frauds among the k nearest.
 * leaves_visited (optional) reports how many leaves were scanned. */
static inline int vk_count_stats(const VkIndex *ix, const float *qf, int k, int budget, int *leaves_visited){
    int16_t q[VKNN_DIM]; for(int j=0;j<VKNN_DIM;j++) q[j]=vk__q(qf[j]);
    int64_t bd[8]; uint8_t bl[8]; if(k>8)k=8;
    for(int i=0;i<8;i++){bd[i]=(int64_t)1<<62;bl[i]=0;}
    int leaves=(budget<=0)?0x7fffffff:budget;
    int visited=0;

    int     sn[VKNN_STACK];
    int64_t sb[VKNN_STACK];
    int sl=0;
    int cur=0;
    int64_t curb=vk__box_lb(&ix->nodes[0],q);
    for(;;){
        if(curb < bd[k-1] && leaves > 0){
            const VkNode *nd=&ix->nodes[cur];
            if(nd->dim<0){
                vk__leaf_scan(ix,nd,q,k,bd,bl);
                leaves--; visited++;
            } else {
                int64_t lb=vk__box_lb(&ix->nodes[nd->left], q);
                int64_t rb=vk__box_lb(&ix->nodes[nd->right],q);
                int     ni,fi; int64_t nb,fb;
                if(lb<=rb){ ni=nd->left; nb=lb; fi=nd->right; fb=rb; }
                else      { ni=nd->right; nb=rb; fi=nd->left;  fb=lb; }
                if(fb < bd[k-1] && sl < VKNN_STACK){ sn[sl]=fi; sb[sl]=fb; sl++; }
                if(nb < bd[k-1]){ cur=ni; curb=nb; continue; }
            }
        }
        if(sl==0) break;
        sl--; cur=sn[sl]; curb=sb[sl];
    }
    if(leaves_visited)*leaves_visited=visited;
    int fr=0; for(int i=0;i<k;i++) fr+=bl[i];
    return fr;
}
static inline int vk_count(const VkIndex *ix, const float *qf, int k, int budget){
    return vk_count_stats(ix,qf,k,budget,NULL);
}
static inline float vk_score(const VkIndex *ix, const float *qf, int k, int budget){
    return (float)vk_count(ix,qf,k,budget) / (float)(k<=8?k:8);
}
static inline void vk_free(VkIndex *ix){ if(ix->map) munmap(ix->map,ix->maplen); ix->map=NULL; }
#endif
