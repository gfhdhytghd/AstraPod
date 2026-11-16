/* Optional deterministic golden trace. No device access. */
#ifndef PODLINK_TRACE_H
#define PODLINK_TRACE_H
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
static void trace(unsigned tag,const void *data,unsigned n) {
 static FILE *file;
 const char *path=getenv("PODLINK_TRACE");
 if(!path)return;
 if(!file){file=fopen(path,"wb");if(!file)abort();}
 fwrite(&tag,4,1,file);fwrite(&n,4,1,file);if(n)fwrite(data,1,n,file);
}
#endif
