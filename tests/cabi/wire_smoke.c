#include <stdint.h>
#include <string.h>
#include "../../src/cabi/fluxa_cabi.h"
int main(void){fluxa_cabi_message m={0};fluxa_cabi_view v;int32_t x=0;fluxa_cabi_message_init(&m);if(!fluxa_cabi_add_int(&m,0x01020304))return 1;v.data=m.data;v.size=m.size;if(!fluxa_cabi_view_validate(&v)||!fluxa_cabi_get_int(&v,0,&x)||x!=0x01020304)return 2;/* magic + version + count + tag/header + i32 LE */{const unsigned char*p=(const unsigned char*)m.data;if(memcmp(p,"FXCB",4)||p[m.size-4]!=0x04||p[m.size-3]!=0x03||p[m.size-2]!=0x02||p[m.size-1]!=0x01)return 3;}fluxa_cabi_message_free(&m);return 0;}
