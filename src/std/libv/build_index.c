/* build_index.c — refs.bin (n,dim header; 14xfloat + 1 byte label records)
 *               -> kdtree.bin in VKN3 format (int16 pts + per-node AABBs). */
#define _POSIX_C_SOURCE 200809L
#include "vknn.h"

int main(int argc, char **argv){
    const char *in  = (argc>1)?argv[1]:"./refs.bin";
    const char *out = (argc>2)?argv[2]:"./kdtree.bin";
    FILE *f=fopen(in,"rb");
    if(!f){ fprintf(stderr,"cannot open %s\n",in); return 1; }
    int32_t n=0, dim=0;
    if(fread(&n,4,1,f)!=1 || fread(&dim,4,1,f)!=1 || dim!=VKNN_DIM || n<=0){
        fprintf(stderr,"bad header (n=%d dim=%d)\n",n,dim); fclose(f); return 1;
    }
    float   *pts=(float*)malloc((size_t)n*VKNN_DIM*sizeof(float));
    uint8_t *lab=(uint8_t*)malloc((size_t)n);
    if(!pts||!lab){ fprintf(stderr,"oom\n"); return 1; }
    /* SoA layout: header, then ALL vectors (n*14 floats), then ALL labels (n bytes) */
    if(fread(pts,sizeof(float),(size_t)n*VKNN_DIM,f)!=(size_t)n*VKNN_DIM){
        fprintf(stderr,"truncated vectors\n"); return 1;
    }
    if(fread(lab,1,(size_t)n,f)!=(size_t)n){
        fprintf(stderr,"truncated labels\n"); return 1;
    }
    long badlab=0; for(int i=0;i<n;i++) if(lab[i]>1) badlab++;
    if(badlab){ fprintf(stderr,"non-binary labels: %ld (wrong layout?)\n",badlab); return 1; }
    fclose(f);
    fprintf(stderr,"loaded %d refs; building VKN3 (AABB) index...\n",n);
    int nodes=vk_build_and_save(pts,lab,n,out);
    fprintf(stderr,"done: %d nodes -> %s\n",nodes,out);
    free(pts); free(lab);
    return nodes>0?0:1;
}
