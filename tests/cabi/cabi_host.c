#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/cabi/fluxa_cabi.h"

static int fail(const char *s){fprintf(stderr,"%s\n",s);return 1;}

int main(int argc,char **argv){
    fluxa_cabi_config cfg; fluxa_cabi_runtime *rt=NULL; fluxa_cabi_error err;
    fluxa_cabi_message req={0}; fluxa_cabi_view reqv,resp;
    int32_t ai[3]={10,-20,30}; double af[3]={1.25,-2.5,3.75}; uint8_t ab[3]={1,0,1};
    fluxa_cabi_str_view as[3]={{"a",1},{"Fluxa",5},{"",0}}, sv;
    int32_t xi; double xf; int xb; uint32_t n; int rc;

    if(argc!=3)return fail("usage: cabi_host ENTRY CONFIG");
    memset(&cfg,0,sizeof(cfg));cfg.struct_size=sizeof(cfg);cfg.abi_version=FLUXA_CABI_ABI_VERSION;cfg.entry_path=argv[1];cfg.config_path=argv[2];cfg.dispatch_fn="cabi_dispatch";
    rc=fluxa_cabi_open(&cfg,&rt,&err);if(rc!=FLUXA_CABI_OK){fprintf(stderr,"open: %u %s\n",err.code,err.message);return 1;}

    fluxa_cabi_message_init(&req);
    if(!fluxa_cabi_add_int(&req,41)||!fluxa_cabi_add_float(&req,2.5)||!fluxa_cabi_add_bool(&req,1)||
       !fluxa_cabi_add_str(&req,"hello",5)||!fluxa_cabi_add_int_arr(&req,ai,3)||
       !fluxa_cabi_add_float_arr(&req,af,3)||!fluxa_cabi_add_bool_arr(&req,ab,3)||
       !fluxa_cabi_add_str_arr(&req,as,3)) return fail("message build failed");
    reqv.data=req.data;reqv.size=req.size;
    if(!fluxa_cabi_view_validate(&reqv)||fluxa_cabi_value_count(&reqv)!=8)return fail("request wire validation failed");

    rc=fluxa_cabi_exchange(rt,&reqv,&resp,&err);if(rc!=FLUXA_CABI_OK){fprintf(stderr,"exchange: %u %s\n",err.code,err.message);return 1;}
    if(fluxa_cabi_value_count(&resp)!=8)return fail("bad response count");
    if(!fluxa_cabi_get_int(&resp,0,&xi)||xi!=42)return fail("bad int response");
    if(!fluxa_cabi_get_float(&resp,1,&xf)||fabs(xf-3.0)>1e-12)return fail("bad float response");
    if(!fluxa_cabi_get_bool(&resp,2,&xb)||xb!=0)return fail("bad bool response");
    if(!fluxa_cabi_get_str(&resp,3,&sv)||sv.size!=5||memcmp(sv.data,"hello",5))return fail("bad str response");
    if(!fluxa_cabi_get_arr_count(&resp,4,&n)||n!=3||!fluxa_cabi_get_int_arr_value(&resp,4,1,&xi)||xi!=-20)return fail("bad int arr response");
    if(!fluxa_cabi_get_arr_count(&resp,5,&n)||n!=3||!fluxa_cabi_get_float_arr_value(&resp,5,2,&xf)||fabs(xf-3.75)>1e-12)return fail("bad float arr response");
    if(!fluxa_cabi_get_arr_count(&resp,6,&n)||n!=3||!fluxa_cabi_get_bool_arr_value(&resp,6,1,&xb)||xb!=0)return fail("bad bool arr response");
    if(!fluxa_cabi_get_arr_count(&resp,7,&n)||n!=3||!fluxa_cabi_get_str_arr_value(&resp,7,1,&sv)||sv.size!=5||memcmp(sv.data,"Fluxa",5))return fail("bad str arr response");

    /* Optional envelope is independently testable and never changes FXCB. */
    if(fluxa_cabi_security_available()){
        uint8_t key[FLUXA_CABI_KEY_BYTES];fluxa_cabi_message sealed={0},clear={0};fluxa_cabi_view x;
        memset(key,0x5a,sizeof(key));fluxa_cabi_message_init(&sealed);fluxa_cabi_message_init(&clear);
        if(fluxa_cabi_seal(key,&reqv,&sealed,&err)!=FLUXA_CABI_OK)return fail("seal failed");
        x.data=sealed.data;x.size=sealed.size;
        if(fluxa_cabi_unseal(key,&x,&clear,&err)!=FLUXA_CABI_OK)return fail("unseal failed");
        if(clear.size!=req.size||memcmp(clear.data,req.data,req.size))return fail("secure roundtrip changed clear frame");
        {
            fluxa_cabi_message sealed_resp={0}, clear_resp={0};
            fluxa_cabi_view sr, cr;
            fluxa_cabi_message_init(&sealed_resp); fluxa_cabi_message_init(&clear_resp);
            if(fluxa_cabi_exchange_sealed(rt,key,&x,&sealed_resp,&err)!=FLUXA_CABI_OK)return fail("secure exchange failed");
            sr.data=sealed_resp.data; sr.size=sealed_resp.size;
            if(fluxa_cabi_unseal(key,&sr,&clear_resp,&err)!=FLUXA_CABI_OK)return fail("secure response unseal failed");
            cr.data=clear_resp.data; cr.size=clear_resp.size;
            if(fluxa_cabi_value_count(&cr)!=8)return fail("secure response changed typed protocol");
            fluxa_cabi_message_free(&sealed_resp); fluxa_cabi_message_free(&clear_resp);
        }
        fluxa_cabi_message_free(&sealed);fluxa_cabi_message_free(&clear);
    }

    fluxa_cabi_message_free(&req);fluxa_cabi_close(rt);puts("CABI_HOST_PASS");return 0;
}
