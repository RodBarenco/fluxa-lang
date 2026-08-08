#include "../../src/cabi/fluxa_cabi.h"
#include <string.h>
int main(void){
    fluxa_cabi_config c; fluxa_cabi_message m; fluxa_cabi_view v;
    memset(&c,0,sizeof(c)); memset(&m,0,sizeof(m)); memset(&v,0,sizeof(v));
    c.struct_size=sizeof(c); c.abi_version=FLUXA_CABI_ABI_VERSION;
    return (FLUXA_CABI_INT==1 && FLUXA_CABI_ARR_STR==8) ? 0 : 1;
}
