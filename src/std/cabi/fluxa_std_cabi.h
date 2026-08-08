#ifndef FLUXA_STD_CABI_H
#define FLUXA_STD_CABI_H

/*
 * std.cabi — Fluxa side of the deterministic typed C ABI bridge.
 *
 * Values crossing the bridge are ONLY:
 *   int, float, bool, str, int arr, float arr, bool arr, str arr.
 * No prst/dyn/Block/pointer/runtime state is part of this protocol.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../scope.h"
#include "../../err.h"
#include "../../cabi/fluxa_cabi.h"
#include "../../cabi/fluxa_cabi_context.h"

static inline Value cabi_nil(void) { Value v; memset(&v,0,sizeof(v)); v.type=VAL_NIL; return v; }
static inline Value cabi_int(long n) { return val_int(n); }
static inline Value cabi_float(double n) { return val_float(n); }
static inline Value cabi_bool(int n) { return val_bool(n?1:0); }
static inline Value cabi_str_copy_n(const char *s, uint32_t n) {
    char *tmp=(char*)malloc((size_t)n+1u); Value v;
    if(!tmp) return cabi_nil();
    if (n) memcpy(tmp, s, n);
    tmp[n] = '\0';
    v = val_string(tmp);
    free(tmp);
    return v;
}

static inline Value fluxa_std_cabi_call(
    const char *fn_name, const Value *args, int argc,
    ErrStack *err, int *had_error, int line)
{
    char errbuf[320];
    const fluxa_cabi_view *req;
    fluxa_cabi_message *resp;

#define CABI_ERR(msg) do { snprintf(errbuf,sizeof(errbuf),"cabi.%s (line %d): %s",fn_name,line,(msg)); errstack_push(err,ERR_FLUXA,errbuf,"cabi",line); *had_error=1; return cabi_nil(); } while(0)
#define NEED(n) do { if(argc!=(n)){snprintf(errbuf,sizeof(errbuf),"cabi.%s: expected %d argument(s), got %d",fn_name,(n),argc);errstack_push(err,ERR_FLUXA,errbuf,"cabi",line);*had_error=1;return cabi_nil();}} while(0)
#define REQUIRE_CTX() do { if(!fluxa_cabi_ctx_active()) CABI_ERR("no active host exchange"); req=fluxa_cabi_ctx_request(); resp=fluxa_cabi_ctx_response(); } while(0)
#define GET_INDEX(i,var) if(args[(i)].type!=VAL_INT) CABI_ERR("index must be int"); long var=args[(i)].as.integer; if(var<0) CABI_ERR("index must be >= 0")
#define CHECK_RESP() do { if(resp->size>fluxa_cabi_ctx_max_frame()) CABI_ERR("response frame exceeds configured limit"); } while(0)

    if(strcmp(fn_name,"version")==0){ NEED(0); return val_string("1.0.0"); }
    if(strcmp(fn_name,"count")==0){ NEED(0); REQUIRE_CTX(); return cabi_int((long)fluxa_cabi_value_count(req)); }
    if(strcmp(fn_name,"type")==0){
        fluxa_cabi_type t; NEED(1); REQUIRE_CTX(); GET_INDEX(0,idx);
        if(!fluxa_cabi_value_type(req,(uint32_t)idx,&t)) CABI_ERR("index out of bounds");
        switch(t){case FLUXA_CABI_INT:return val_string("int");case FLUXA_CABI_FLOAT:return val_string("float");case FLUXA_CABI_BOOL:return val_string("bool");case FLUXA_CABI_STR:return val_string("str");case FLUXA_CABI_ARR_INT:return val_string("arr<int>");case FLUXA_CABI_ARR_FLOAT:return val_string("arr<float>");case FLUXA_CABI_ARR_BOOL:return val_string("arr<bool>");case FLUXA_CABI_ARR_STR:return val_string("arr<str>");default:CABI_ERR("invalid type tag");}
    }
    if(strcmp(fn_name,"read_int")==0){int32_t x;NEED(1);REQUIRE_CTX();GET_INDEX(0,idx);if(!fluxa_cabi_get_int(req,(uint32_t)idx,&x))CABI_ERR("value is not int or index is invalid");return cabi_int((long)x);}
    if(strcmp(fn_name,"read_float")==0){double x;NEED(1);REQUIRE_CTX();GET_INDEX(0,idx);if(!fluxa_cabi_get_float(req,(uint32_t)idx,&x))CABI_ERR("value is not float or index is invalid");return cabi_float(x);}
    if(strcmp(fn_name,"read_bool")==0){int x;NEED(1);REQUIRE_CTX();GET_INDEX(0,idx);if(!fluxa_cabi_get_bool(req,(uint32_t)idx,&x))CABI_ERR("value is not bool or index is invalid");return cabi_bool(x);}
    if(strcmp(fn_name,"read_str")==0){fluxa_cabi_str_view s;Value v;NEED(1);REQUIRE_CTX();GET_INDEX(0,idx);if(!fluxa_cabi_get_str(req,(uint32_t)idx,&s))CABI_ERR("value is not str or index is invalid");if(memchr(s.data,'\0',s.size))CABI_ERR("wire str contains NUL, unsupported by Fluxa str");v=cabi_str_copy_n(s.data,s.size);if(v.type!=VAL_STRING)CABI_ERR("out of memory");return v;}

    if(strcmp(fn_name,"read_int_arr")==0){
        uint32_t n,i;fluxa_cabi_type tt;NEED(2);REQUIRE_CTX();GET_INDEX(0,idx);
        if(args[1].type!=VAL_ARR) CABI_ERR("destination must be int arr");
        if(!fluxa_cabi_value_type(req,(uint32_t)idx,&tt)||tt!=FLUXA_CABI_ARR_INT||!fluxa_cabi_get_arr_count(req,(uint32_t)idx,&n))CABI_ERR("array type mismatch or invalid index");
        if((uint32_t)args[1].as.arr.size!=n) CABI_ERR("destination int arr size does not match wire array size");
        for(i=0;i<n;i++){int32_t z;if(args[1].as.arr.data[i].type!=VAL_INT)CABI_ERR("destination contains non-int element");if(!fluxa_cabi_get_int_arr_value(req,(uint32_t)idx,i,&z))CABI_ERR("invalid int array element");args[1].as.arr.data[i]=val_int((long)z);}return cabi_nil();
    }
    if(strcmp(fn_name,"read_float_arr")==0){
        uint32_t n,i;fluxa_cabi_type tt;NEED(2);REQUIRE_CTX();GET_INDEX(0,idx);
        if(args[1].type!=VAL_ARR) CABI_ERR("destination must be float arr");
        if(!fluxa_cabi_value_type(req,(uint32_t)idx,&tt)||tt!=FLUXA_CABI_ARR_FLOAT||!fluxa_cabi_get_arr_count(req,(uint32_t)idx,&n))CABI_ERR("array type mismatch or invalid index");
        if((uint32_t)args[1].as.arr.size!=n) CABI_ERR("destination float arr size does not match wire array size");
        for(i=0;i<n;i++){double z;if(args[1].as.arr.data[i].type!=VAL_FLOAT)CABI_ERR("destination contains non-float element");if(!fluxa_cabi_get_float_arr_value(req,(uint32_t)idx,i,&z))CABI_ERR("invalid float array element");args[1].as.arr.data[i]=val_float(z);}return cabi_nil();
    }
    if(strcmp(fn_name,"read_bool_arr")==0){
        uint32_t n,i;fluxa_cabi_type tt;NEED(2);REQUIRE_CTX();GET_INDEX(0,idx);
        if(args[1].type!=VAL_ARR) CABI_ERR("destination must be bool arr");
        if(!fluxa_cabi_value_type(req,(uint32_t)idx,&tt)||tt!=FLUXA_CABI_ARR_BOOL||!fluxa_cabi_get_arr_count(req,(uint32_t)idx,&n))CABI_ERR("array type mismatch or invalid index");
        if((uint32_t)args[1].as.arr.size!=n) CABI_ERR("destination bool arr size does not match wire array size");
        for(i=0;i<n;i++){int z;if(args[1].as.arr.data[i].type!=VAL_BOOL)CABI_ERR("destination contains non-bool element");if(!fluxa_cabi_get_bool_arr_value(req,(uint32_t)idx,i,&z))CABI_ERR("invalid bool array element");args[1].as.arr.data[i]=val_bool(z);}return cabi_nil();
    }
    if(strcmp(fn_name,"read_str_arr")==0){
        uint32_t n,i;fluxa_cabi_type tt;NEED(2);REQUIRE_CTX();GET_INDEX(0,idx);
        if(args[1].type!=VAL_ARR) CABI_ERR("destination must be str arr");
        if(!fluxa_cabi_value_type(req,(uint32_t)idx,&tt)||tt!=FLUXA_CABI_ARR_STR||!fluxa_cabi_get_arr_count(req,(uint32_t)idx,&n))CABI_ERR("array type mismatch or invalid index");
        if((uint32_t)args[1].as.arr.size!=n) CABI_ERR("destination str arr size does not match wire array size");
        for(i=0;i<n;i++){
            fluxa_cabi_str_view sv;Value nv;
            if(args[1].as.arr.data[i].type!=VAL_STRING)CABI_ERR("destination contains non-str element");
            if(!fluxa_cabi_get_str_arr_value(req,(uint32_t)idx,i,&sv)||memchr(sv.data,'\0',sv.size))CABI_ERR("invalid str array element");
            nv=cabi_str_copy_n(sv.data,sv.size);if(nv.type!=VAL_STRING)CABI_ERR("out of memory");
            value_release_data(&args[1].as.arr.data[i]);
            args[1].as.arr.data[i]=nv;
        }
        return cabi_nil();
    }

    if(strcmp(fn_name,"response_reset")==0){NEED(0);REQUIRE_CTX();fluxa_cabi_message_reset(resp);return cabi_nil();}
    if(strcmp(fn_name,"write_int")==0){NEED(1);REQUIRE_CTX();if(args[0].type!=VAL_INT)CABI_ERR("expected int");if((int64_t)args[0].as.integer<INT32_MIN||(int64_t)args[0].as.integer>INT32_MAX)CABI_ERR("int outside deterministic i32 wire range");if(!fluxa_cabi_add_int(resp,(int32_t)args[0].as.integer))CABI_ERR("could not append int");CHECK_RESP();return cabi_nil();}
    if(strcmp(fn_name,"write_float")==0){NEED(1);REQUIRE_CTX();if(args[0].type!=VAL_FLOAT)CABI_ERR("expected float");if(!fluxa_cabi_add_float(resp,args[0].as.real))CABI_ERR("could not append float");CHECK_RESP();return cabi_nil();}
    if(strcmp(fn_name,"write_bool")==0){NEED(1);REQUIRE_CTX();if(args[0].type!=VAL_BOOL)CABI_ERR("expected bool");if(!fluxa_cabi_add_bool(resp,args[0].as.boolean))CABI_ERR("could not append bool");CHECK_RESP();return cabi_nil();}
    if(strcmp(fn_name,"write_str")==0){size_t n;NEED(1);REQUIRE_CTX();if(args[0].type!=VAL_STRING||!args[0].as.string)CABI_ERR("expected str");n=strlen(args[0].as.string);if(n>UINT32_MAX)CABI_ERR("str too large");if(!fluxa_cabi_add_str(resp,args[0].as.string,(uint32_t)n))CABI_ERR("could not append str");CHECK_RESP();return cabi_nil();}

#define ARR_ARG() NEED(1); REQUIRE_CTX(); if(args[0].type!=VAL_ARR) CABI_ERR("expected arr")
    if(strcmp(fn_name,"write_int_arr")==0){uint32_t i,n;int32_t *a;ARR_ARG();n=(uint32_t)args[0].as.arr.size;a=(int32_t*)malloc((n?n:1)*sizeof(*a));if(!a)CABI_ERR("out of memory");for(i=0;i<n;i++){Value v=args[0].as.arr.data[i];if(v.type!=VAL_INT||(int64_t)v.as.integer<INT32_MIN||(int64_t)v.as.integer>INT32_MAX){free(a);CABI_ERR("arr contains non-int or out-of-range int");}a[i]=(int32_t)v.as.integer;}i=fluxa_cabi_add_int_arr(resp,a,n);free(a);if(!i)CABI_ERR("could not append int arr");CHECK_RESP();return cabi_nil();}
    if(strcmp(fn_name,"write_float_arr")==0){uint32_t i,n;double *a;ARR_ARG();n=(uint32_t)args[0].as.arr.size;a=(double*)malloc((n?n:1)*sizeof(*a));if(!a)CABI_ERR("out of memory");for(i=0;i<n;i++){Value v=args[0].as.arr.data[i];if(v.type!=VAL_FLOAT){free(a);CABI_ERR("arr contains non-float");}a[i]=v.as.real;}i=fluxa_cabi_add_float_arr(resp,a,n);free(a);if(!i)CABI_ERR("could not append float arr");CHECK_RESP();return cabi_nil();}
    if(strcmp(fn_name,"write_bool_arr")==0){uint32_t i,n;uint8_t *a;ARR_ARG();n=(uint32_t)args[0].as.arr.size;a=(uint8_t*)malloc(n?n:1);if(!a)CABI_ERR("out of memory");for(i=0;i<n;i++){Value v=args[0].as.arr.data[i];if(v.type!=VAL_BOOL){free(a);CABI_ERR("arr contains non-bool");}a[i]=(uint8_t)(v.as.boolean?1:0);}i=fluxa_cabi_add_bool_arr(resp,a,n);free(a);if(!i)CABI_ERR("could not append bool arr");CHECK_RESP();return cabi_nil();}
    if(strcmp(fn_name,"write_str_arr")==0){uint32_t i,n;fluxa_cabi_str_view *a;ARR_ARG();n=(uint32_t)args[0].as.arr.size;a=(fluxa_cabi_str_view*)calloc(n?n:1,sizeof(*a));if(!a)CABI_ERR("out of memory");for(i=0;i<n;i++){Value v=args[0].as.arr.data[i];size_t z;if(v.type!=VAL_STRING||!v.as.string){free(a);CABI_ERR("arr contains non-str");}z=strlen(v.as.string);if(z>UINT32_MAX){free(a);CABI_ERR("str element too large");}a[i].data=v.as.string;a[i].size=(uint32_t)z;}i=fluxa_cabi_add_str_arr(resp,a,n);free(a);if(!i)CABI_ERR("could not append str arr");CHECK_RESP();return cabi_nil();}
#undef ARR_ARG

#undef CHECK_RESP
#undef GET_INDEX
#undef REQUIRE_CTX
#undef NEED
#undef CABI_ERR

    snprintf(errbuf,sizeof(errbuf),"cabi.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"cabi",line);*had_error=1;return cabi_nil();
}

FLUXA_LIB_EXPORT(
    name      = "cabi",
    toml_key  = "std.cabi",
    owner     = "cabi",
    call      = fluxa_std_cabi_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif
