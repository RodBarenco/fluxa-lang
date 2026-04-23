#ifndef FLUXA_STD_LIBV_H
#define FLUXA_STD_LIBV_H

/*
 * std.libv — N-dimensional vector, matrix, tensor math for Fluxa-lang
 *
 * GLM-inspired API. No new types — all storage is float arr or int arr
 * (flat, col-major). Operations add shape semantics on top.
 * Pure C99, zero external deps. Works on RP2040 and ESP32.
 *
 * API:
 *   Initializers: vec2/vec3/vec4/mat2/mat3/mat4/ivec2/ivec3/vec(n)/mat(r,c)/tens(d...)
 *   Vector ops:   add, sub, scale, normalize, negate, lerp, dot, norm, angle, cross
 *   Matrix ops:   matmul, transpose, identity, inverse, det
 *   Transforms:   translate, rotate, scale_mat, perspective, ortho, lookat
 *   Tensor ops:   tens_add, tens_scale, tens_slice
 *   Utilities:    fill, copy, eq, shape
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../scope.h"
#include "../../err.h"

#ifdef FLUXA_LIBV_BLAS
#  include <cblas.h>
#endif

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value lv_nil(void)       { Value v; v.type = VAL_NIL;   return v; }
static inline Value lv_float(double d) { Value v; v.type = VAL_FLOAT; v.as.real    = d; return v; }
static inline Value lv_int(long n)     { Value v; v.type = VAL_INT;   v.as.integer = n; return v; }
static inline Value lv_bool(int b)     { Value v; v.type = VAL_BOOL;  v.as.boolean = b; return v; }

/* Build a float arr of given size, initialized to zeros */
static inline Value lv_make_farr(int n) {
    Value *data = (Value *)calloc((size_t)n, sizeof(Value));
    for (int i = 0; i < n; i++) { data[i].type = VAL_FLOAT; data[i].as.real = 0.0; }
    return val_arr(data, n);
}

/* Build an int arr of given size, initialized to zeros */
static inline Value lv_make_iarr(int n) {
    Value *data = (Value *)calloc((size_t)n, sizeof(Value));
    for (int i = 0; i < n; i++) { data[i].type = VAL_INT; data[i].as.integer = 0; }
    return val_arr(data, n);
}

/* Set identity matrix in float arr (col-major, n×n) */
static inline void lv_set_identity(Value *data, int n) {
    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++) {
            data[c*n+r].type   = VAL_FLOAT;
            data[c*n+r].as.real = (r == c) ? 1.0 : 0.0;
        }
}

/* Get float element — handles both VAL_FLOAT and VAL_INT */
static inline double lv_get(const Value *data, int i) {
    if (data[i].type == VAL_FLOAT) return data[i].as.real;
    if (data[i].type == VAL_INT)   return (double)data[i].as.integer;
    return 0.0;
}
static inline void lv_setf(Value *data, int i, double v) {
    data[i].type = VAL_FLOAT; data[i].as.real = v;
}

/* Validate that Value is a float arr; returns data pointer or NULL */
static inline Value *lv_arr(const Value *v, ErrStack *err, int *had_error,
                              int line, const char *ctx, int *sz) {
    char errbuf[280];
    if (v->type != VAL_ARR || !v->as.arr.data) {
        snprintf(errbuf, sizeof(errbuf), "libv.%s: expected float arr", ctx);
        errstack_push(err, ERR_FLUXA, errbuf, "libv", line);
        *had_error = 1; return NULL;
    }
    *sz = v->as.arr.size;
    return v->as.arr.data;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_libv_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int line,
                                         const FluxaConfig *cfg) {
    int use_blas = 0;
    (void)use_blas;
#ifdef FLUXA_LIBV_BLAS
    if (cfg && strncmp(cfg->libv_backend, "blas", 4) == 0) use_blas = 1;
#else
    (void)cfg;
#endif
    char errbuf[280];

#define LV_ERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "libv.%s (line %d): %s", fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "libv", line); \
    *had_error = 1; return lv_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "libv.%s: expected %d arg(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "libv", line); \
        *had_error = 1; return lv_nil(); \
    } \
} while(0)

#define GET_ARR(idx, dptr, sz) \
    int sz = 0; \
    Value *(dptr) = lv_arr(&args[(idx)], err, had_error, line, fn_name, &sz); \
    if (!(dptr)) return lv_nil();

#define GET_FLOAT(idx, var) \
    double (var) = 0.0; \
    if (args[(idx)].type == VAL_FLOAT) (var) = args[(idx)].as.real; \
    else if (args[(idx)].type == VAL_INT) (var) = (double)args[(idx)].as.integer; \
    else LV_ERR("expected float argument");

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT) LV_ERR("expected int argument"); \
    long (var) = args[(idx)].as.integer;

#define SHAPE_EQ(na, nb) do { \
    if ((na) != (nb)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "libv.%s: shape mismatch (%d != %d)", fn_name, (na), (nb)); \
        errstack_push(err, ERR_FLUXA, errbuf, "libv", line); \
        *had_error = 1; return lv_nil(); \
    } \
} while(0)

    /* ── Initializers ────────────────────────────────────── */
    if (!strcmp(fn_name,"vec2"))  return lv_make_farr(2);
    if (!strcmp(fn_name,"vec3"))  return lv_make_farr(3);
    if (!strcmp(fn_name,"vec4"))  return lv_make_farr(4);
    if (!strcmp(fn_name,"ivec2")) return lv_make_iarr(2);
    if (!strcmp(fn_name,"ivec3")) return lv_make_iarr(3);
    if (!strcmp(fn_name,"mat2"))  { Value v=lv_make_farr(4);  lv_set_identity(v.as.arr.data,2); return v; }
    if (!strcmp(fn_name,"mat3"))  { Value v=lv_make_farr(9);  lv_set_identity(v.as.arr.data,3); return v; }
    if (!strcmp(fn_name,"mat4"))  { Value v=lv_make_farr(16); lv_set_identity(v.as.arr.data,4); return v; }

    if (!strcmp(fn_name,"vec")) {
        NEED(1); GET_INT(0,n);
        if (n<1||n>65536) LV_ERR("vec: size out of range [1,65536]");
        return lv_make_farr((int)n);
    }
    if (!strcmp(fn_name,"mat")) {
        NEED(2); GET_INT(0,rows); GET_INT(1,cols);
        if (rows<1||cols<1||rows*cols>65536) LV_ERR("mat: dimensions out of range");
        return lv_make_farr((int)(rows*cols));
    }
    if (!strcmp(fn_name,"tens")) {
        if (argc<1) LV_ERR("tens: requires at least 1 dimension");
        long total=1;
        for (int i=0;i<argc;i++) {
            if (args[i].type!=VAL_INT) LV_ERR("tens: dimensions must be int");
            long d=args[i].as.integer;
            if (d<1) LV_ERR("tens: dimension must be >= 1");
            total*=d; if (total>65536) LV_ERR("tens: total size exceeds 65536");
        }
        return lv_make_farr((int)total);
    }

    /* ── Utilities ───────────────────────────────────────── */
    if (!strcmp(fn_name,"shape")) {
        NEED(1); GET_ARR(0,d,n); return lv_int(n);
    }
    if (!strcmp(fn_name,"fill")) {
        NEED(2); GET_ARR(0,d,n); GET_FLOAT(1,val);
        { for (int i=0;i<n;i++) lv_setf(d,i,val); }
        return lv_nil();
    }
    if (!strcmp(fn_name,"copy")) {
        NEED(2); GET_ARR(0,dst,nd); GET_ARR(1,src,ns);
        SHAPE_EQ(nd,ns);
        { for (int i=0;i<nd;i++) lv_setf(dst,i,lv_get(src,i)); }
        return lv_nil();
    }
    if (!strcmp(fn_name,"eq")) {
        NEED(2); GET_ARR(0,a,na); GET_ARR(1,b,nb);
        if (na!=nb) return lv_bool(0);
        for (int i=0;i<na;i++) {
            double diff=lv_get(a,i)-lv_get(b,i);
            if (diff<-1e-6||diff>1e-6) return lv_bool(0);
        }
        return lv_bool(1);
    }

    /* ── Vector operations ───────────────────────────────── */
    if (!strcmp(fn_name,"add")) {
        NEED(2); GET_ARR(0,a,na); GET_ARR(1,b,nb); SHAPE_EQ(na,nb);
        { for (int i=0;i<na;i++) lv_setf(a,i,lv_get(a,i)+lv_get(b,i)); }
        return lv_nil();
    }
    if (!strcmp(fn_name,"sub")) {
        NEED(2); GET_ARR(0,a,na); GET_ARR(1,b,nb); SHAPE_EQ(na,nb);
        { for (int i=0;i<na;i++) lv_setf(a,i,lv_get(a,i)-lv_get(b,i)); }
        return lv_nil();
    }
    if (!strcmp(fn_name,"scale")) {
        NEED(2); GET_ARR(0,a,na); GET_FLOAT(1,s);
        { for (int i=0;i<na;i++) lv_setf(a,i,lv_get(a,i)*s); }
        return lv_nil();
    }
    if (!strcmp(fn_name,"negate")) {
        NEED(1); GET_ARR(0,a,na);
        { for (int i=0;i<na;i++) lv_setf(a,i,-lv_get(a,i)); }
        return lv_nil();
    }
    if (!strcmp(fn_name,"norm")) {
        NEED(1); GET_ARR(0,a,na);
        double s=0.0; for (int i=0;i<na;i++) { double x=lv_get(a,i); s+=x*x; }
        return lv_float(sqrt(s));
    }
    if (!strcmp(fn_name,"normalize")) {
        NEED(1); GET_ARR(0,a,na);
        double s=0.0; for (int i=0;i<na;i++) { double x=lv_get(a,i); s+=x*x; }
        double len=sqrt(s);
        if (len<1e-12) LV_ERR("normalize: zero vector");
        { for (int i=0;i<na;i++) lv_setf(a,i,lv_get(a,i)/len); }
        return lv_nil();
    }
    if (!strcmp(fn_name,"dot")) {
        NEED(2); GET_ARR(0,a,na); GET_ARR(1,b,nb); SHAPE_EQ(na,nb);
        double d=0.0; for (int i=0;i<na;i++) d+=lv_get(a,i)*lv_get(b,i);
        return lv_float(d);
    }
    if (!strcmp(fn_name,"angle")) {
        NEED(2); GET_ARR(0,a,na); GET_ARR(1,b,nb); SHAPE_EQ(na,nb);
        double da=0.0,db=0.0,dot=0.0;
        for (int i=0;i<na;i++) {
            double ai=lv_get(a,i),bi=lv_get(b,i);
            dot+=ai*bi; da+=ai*ai; db+=bi*bi;
        }
        double denom=sqrt(da)*sqrt(db);
        if (denom<1e-12) LV_ERR("angle: zero vector");
        double c=dot/denom;
        if (c >  1.0) c =  1.0;
        if (c < -1.0) c = -1.0;
        return lv_float(acos(c));
    }
    if (!strcmp(fn_name,"lerp")) {
        NEED(3); GET_ARR(0,a,na); GET_ARR(1,b,nb); GET_FLOAT(2,t); SHAPE_EQ(na,nb);
        for (int i=0;i<na;i++) lv_setf(a,i,lv_get(a,i)*(1.0-t)+lv_get(b,i)*t);
        return lv_nil();
    }
    if (!strcmp(fn_name,"cross")) {
        NEED(3); GET_ARR(0,out,no); GET_ARR(1,a,na); GET_ARR(2,b,nb);
        if (no!=3||na!=3||nb!=3) LV_ERR("cross: all arrays must be vec3");
        double ax=lv_get(a,0),ay=lv_get(a,1),az=lv_get(a,2);
        double bx=lv_get(b,0),by=lv_get(b,1),bz=lv_get(b,2);
        lv_setf(out,0,ay*bz-az*by);
        lv_setf(out,1,az*bx-ax*bz);
        lv_setf(out,2,ax*by-ay*bx);
        return lv_nil();
    }

    /* ── Matrix operations ───────────────────────────────── */
    if (!strcmp(fn_name,"identity")) {
        NEED(1); GET_ARR(0,m,nm);
        int n=(int)round(sqrt((double)nm));
        if (n*n!=nm) LV_ERR("identity: array is not square");
        lv_set_identity(m,n); return lv_nil();
    }
    if (!strcmp(fn_name,"transpose")) {
        NEED(1); GET_ARR(0,m,nm);
        int n=(int)round(sqrt((double)nm));
        if (n*n!=nm) LV_ERR("transpose: array is not square");
        for (int r=0;r<n;r++)
            for (int c=r+1;c<n;c++) {
                double tmp=lv_get(m,c*n+r);
                lv_setf(m,c*n+r,lv_get(m,r*n+c));
                lv_setf(m,r*n+c,tmp);
            }
        return lv_nil();
    }
    if (!strcmp(fn_name,"matmul")) {
        NEED(3); GET_ARR(0,out,no); GET_ARR(1,a,na); GET_ARR(2,b,nb);
        if (na!=nb||na!=no) LV_ERR("matmul: all matrices must be same square size");
        int n=(int)round(sqrt((double)na));
        if (n*n!=na) LV_ERR("matmul: non-square matrix");
#ifdef FLUXA_LIBV_BLAS
        if (use_blas) {
            /* BLAS dgemm: C = alpha*A*B + beta*C, col-major */
            double *da=(double*)malloc((size_t)(n*n)*sizeof(double));
            double *db=(double*)malloc((size_t)(n*n)*sizeof(double));
            double *dc=(double*)calloc((size_t)(n*n),sizeof(double));
            for (int i=0;i<n*n;i++) { da[i]=lv_get(a,i); db[i]=lv_get(b,i); }
            cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
                        n, n, n, 1.0, da, n, db, n, 0.0, dc, n);
            for (int i=0;i<n*n;i++) lv_setf(out,i,dc[i]);
            free(da); free(db); free(dc);
            return lv_nil();
        }
#endif
        double *tmp=(double*)calloc((size_t)(n*n),sizeof(double));
        for (int col=0;col<n;col++)
            for (int row=0;row<n;row++) {
                double s=0.0;
                for (int k=0;k<n;k++) s+=lv_get(a,k*n+row)*lv_get(b,col*n+k);
                tmp[col*n+row]=s;
            }
        for (int i=0;i<n*n;i++) lv_setf(out,i,tmp[i]);
        free(tmp); return lv_nil();
    }
    if (!strcmp(fn_name,"det")) {
        NEED(1); GET_ARR(0,m,nm);
        int n=(int)round(sqrt((double)nm));
        if (n*n!=nm) LV_ERR("det: array is not square");
        if (n==2) return lv_float(lv_get(m,0)*lv_get(m,3)-lv_get(m,2)*lv_get(m,1));
        if (n==3) {
            double a=lv_get(m,0),b=lv_get(m,3),c=lv_get(m,6),
                   d=lv_get(m,1),e=lv_get(m,4),f=lv_get(m,7),
                   g=lv_get(m,2),h=lv_get(m,5),k=lv_get(m,8);
            return lv_float(a*(e*k-f*h)-b*(d*k-f*g)+c*(d*h-e*g));
        }
        if (n==4) {
            double m00=lv_get(m,0),m10=lv_get(m,4),m20=lv_get(m,8), m30=lv_get(m,12),
                   m01=lv_get(m,1),m11=lv_get(m,5),m21=lv_get(m,9), m31=lv_get(m,13),
                   m02=lv_get(m,2),m12=lv_get(m,6),m22=lv_get(m,10),m32=lv_get(m,14),
                   m03=lv_get(m,3),m13=lv_get(m,7),m23=lv_get(m,11),m33=lv_get(m,15);
            return lv_float(
                m00*(m11*(m22*m33-m23*m32)-m12*(m21*m33-m23*m31)+m13*(m21*m32-m22*m31))
               -m10*(m01*(m22*m33-m23*m32)-m02*(m21*m33-m23*m31)+m03*(m21*m32-m22*m31))
               +m20*(m01*(m12*m33-m13*m32)-m02*(m11*m33-m13*m31)+m03*(m11*m32-m12*m31))
               -m30*(m01*(m12*m23-m13*m22)-m02*(m11*m23-m13*m21)+m03*(m11*m22-m12*m21)));
        }
        LV_ERR("det: only 2×2, 3×3, 4×4 supported");
    }
    if (!strcmp(fn_name,"inverse")) {
        NEED(2); GET_ARR(0,out,no); GET_ARR(1,m,nm); SHAPE_EQ(no,nm);
        int n=(int)round(sqrt((double)nm));
        if (n*n!=nm) LV_ERR("inverse: array is not square");
        if (n==2) {
            double a=lv_get(m,0),b=lv_get(m,2),c=lv_get(m,1),d=lv_get(m,3);
            double det=a*d-b*c;
            if (fabs(det)<1e-12) LV_ERR("inverse: matrix is singular");
            lv_setf(out,0,d/det); lv_setf(out,2,-b/det);
            lv_setf(out,1,-c/det); lv_setf(out,3,a/det);
            return lv_nil();
        }
        LV_ERR("inverse: only 2×2 supported (higher dims in std.libdsp)");
    }

    /* ── 3D Transform helpers ────────────────────────────── */
    if (!strcmp(fn_name,"translate")) {
        NEED(4); GET_ARR(0,m,nm); GET_FLOAT(1,tx); GET_FLOAT(2,ty); GET_FLOAT(3,tz);
        if (nm!=16) LV_ERR("translate: expected mat4 (16 elements)");
        double r12=lv_get(m,0)*tx+lv_get(m,4)*ty+lv_get(m,8)*tz +lv_get(m,12);
        double r13=lv_get(m,1)*tx+lv_get(m,5)*ty+lv_get(m,9)*tz +lv_get(m,13);
        double r14=lv_get(m,2)*tx+lv_get(m,6)*ty+lv_get(m,10)*tz+lv_get(m,14);
        lv_setf(m,12,r12); lv_setf(m,13,r13); lv_setf(m,14,r14);
        return lv_nil();
    }
    if (!strcmp(fn_name,"scale_mat")) {
        NEED(4); GET_ARR(0,m,nm); GET_FLOAT(1,sx); GET_FLOAT(2,sy); GET_FLOAT(3,sz);
        if (nm!=16) LV_ERR("scale_mat: expected mat4 (16 elements)");
        for (int i=0;i<4;i++) {
            lv_setf(m,i,   lv_get(m,i)   *sx);
            lv_setf(m,4+i, lv_get(m,4+i) *sy);
            lv_setf(m,8+i, lv_get(m,8+i) *sz);
        }
        return lv_nil();
    }
    if (!strcmp(fn_name,"rotate")) {
        NEED(5); GET_ARR(0,m,nm); GET_FLOAT(1,angle);
        GET_FLOAT(2,ax); GET_FLOAT(3,ay); GET_FLOAT(4,az);
        if (nm!=16) LV_ERR("rotate: expected mat4 (16 elements)");
        double len=sqrt(ax*ax+ay*ay+az*az);
        if (len<1e-12) LV_ERR("rotate: zero axis vector");
        ax/=len; ay/=len; az/=len;
        double c=cos(angle),s=sin(angle),t=1.0-c;
        double R[16]={
            t*ax*ax+c,    t*ax*ay+s*az, t*ax*az-s*ay, 0,
            t*ax*ay-s*az, t*ay*ay+c,    t*ay*az+s*ax, 0,
            t*ax*az+s*ay, t*ay*az-s*ax, t*az*az+c,    0,
            0, 0, 0, 1
        };
        double tmp[16];
        for (int col=0;col<4;col++)
            for (int row=0;row<4;row++) {
                double sum=0.0;
                for (int k=0;k<4;k++) sum+=lv_get(m,k*4+row)*R[col*4+k];
                tmp[col*4+row]=sum;
            }
        for (int i=0;i<16;i++) lv_setf(m,i,tmp[i]);
        return lv_nil();
    }
    if (!strcmp(fn_name,"perspective")) {
        NEED(5); GET_ARR(0,m,nm);
        GET_FLOAT(1,fov); GET_FLOAT(2,aspect); GET_FLOAT(3,near); GET_FLOAT(4,far);
        if (nm!=16) LV_ERR("perspective: expected mat4");
        double f=1.0/tan(fov*0.5), nf=1.0/(near-far);
        for (int i=0;i<16;i++) lv_setf(m,i,0.0);
        lv_setf(m,0,f/aspect); lv_setf(m,5,f);
        lv_setf(m,10,(far+near)*nf); lv_setf(m,11,-1.0);
        lv_setf(m,14,2.0*far*near*nf);
        return lv_nil();
    }
    if (!strcmp(fn_name,"ortho")) {
        NEED(7); GET_ARR(0,m,nm);
        GET_FLOAT(1,left); GET_FLOAT(2,right); GET_FLOAT(3,bottom);
        GET_FLOAT(4,top);  GET_FLOAT(5,near);  GET_FLOAT(6,far);
        if (nm!=16) LV_ERR("ortho: expected mat4");
        double lr=1.0/(right-left),bt=1.0/(top-bottom),fn2=1.0/(far-near);
        for (int i=0;i<16;i++) lv_setf(m,i,0.0);
        lv_setf(m,0,2.0*lr); lv_setf(m,5,2.0*bt); lv_setf(m,10,-2.0*fn2);
        lv_setf(m,12,-(right+left)*lr); lv_setf(m,13,-(top+bottom)*bt);
        lv_setf(m,14,-(far+near)*fn2); lv_setf(m,15,1.0);
        return lv_nil();
    }
    if (!strcmp(fn_name,"lookat")) {
        NEED(4); GET_ARR(0,m,nm); GET_ARR(1,eye,ne); GET_ARR(2,center,nc); GET_ARR(3,up,nu);
        if (nm!=16) LV_ERR("lookat: expected mat4");
        if (ne<3||nc<3||nu<3) LV_ERR("lookat: eye/center/up must be vec3+");
        double ex=lv_get(eye,0),ey=lv_get(eye,1),ez=lv_get(eye,2);
        double cx=lv_get(center,0),cy=lv_get(center,1),cz=lv_get(center,2);
        double ux=lv_get(up,0),uy=lv_get(up,1),uz=lv_get(up,2);
        double fx=ex-cx,fy=ey-cy,fz=ez-cz;
        double fl=sqrt(fx*fx+fy*fy+fz*fz);
        if (fl<1e-12) LV_ERR("lookat: eye == center");
        fx/=fl; fy/=fl; fz/=fl;
        double rx=fy*uz-fz*uy,ry=fz*ux-fx*uz,rz=fx*uy-fy*ux;
        double rl=sqrt(rx*rx+ry*ry+rz*rz);
        if (rl<1e-12) LV_ERR("lookat: forward parallel to up");
        rx/=rl; ry/=rl; rz/=rl;
        double ux2=ry*fz-rz*fy,uy2=rz*fx-rx*fz,uz2=rx*fy-ry*fx;
        for (int i=0;i<16;i++) lv_setf(m,i,0.0);
        lv_setf(m,0,rx); lv_setf(m,1,ux2); lv_setf(m,2,fx);
        lv_setf(m,4,ry); lv_setf(m,5,uy2); lv_setf(m,6,fy);
        lv_setf(m,8,rz); lv_setf(m,9,uz2); lv_setf(m,10,fz);
        lv_setf(m,12,-(rx*ex+ry*ey+rz*ez));
        lv_setf(m,13,-(ux2*ex+uy2*ey+uz2*ez));
        lv_setf(m,14,-(fx*ex+fy*ey+fz*ez));
        lv_setf(m,15,1.0);
        return lv_nil();
    }

    /* ── Tensor operations ───────────────────────────────── */
    if (!strcmp(fn_name,"tens_add")) {
        NEED(2); GET_ARR(0,t,nt); GET_ARR(1,t2,nt2); SHAPE_EQ(nt,nt2);
        for (int i=0;i<nt;i++) lv_setf(t,i,lv_get(t,i)+lv_get(t2,i));
        return lv_nil();
    }
    if (!strcmp(fn_name,"tens_scale")) {
        NEED(2); GET_ARR(0,t,nt); GET_FLOAT(1,s);
        { for (int i=0;i<nt;i++) lv_setf(t,i,lv_get(t,i)*s); }
        return lv_nil();
    }
    if (!strcmp(fn_name,"tens_slice")) {
        NEED(3); GET_ARR(0,out,no); GET_ARR(1,t,nt); GET_INT(2,idx);
        if (nt%no!=0) LV_ERR("tens_slice: out size must divide tensor size evenly");
        if (idx<0||(int)idx*no+no>nt) LV_ERR("tens_slice: index out of range");
        for (int i=0;i<no;i++) lv_setf(out,i,lv_get(t,(int)idx*no+i));
        return lv_nil();
    }

#undef LV_ERR
#undef NEED
#undef GET_ARR
#undef GET_FLOAT
#undef GET_INT
#undef SHAPE_EQ

    snprintf(errbuf, sizeof(errbuf), "libv.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "libv", line);
    *had_error = 1; return lv_nil();
}

/* ── Lib descriptor ──────────────────────────────────────────────── */
FLUXA_LIB_EXPORT(
    name      = "libv",
    toml_key  = "std.libv",
    owner     = "libv",
    call      = fluxa_std_libv_call,
    rt_aware  = 0,
    cfg_aware = 1
)

#endif /* FLUXA_STD_LIBV_H */
