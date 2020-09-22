#ifndef LOG_H
#define LOG_H

#include <libpmemobj.h>

POBJ_LAYOUT_BEGIN(LOG);
POBJ_LAYOUT_TOID(LOG, char);
POBJ_LAYOUT_END(LOG);

#endif

