#pragma once

#include "../external/wasm3.h"

#ifdef __cplusplus
extern "C" {
#endif

int m3l_getGlobalCount(IM3Module module);
const char* m3l_getGlobalName(IM3Module module, int idx);

#ifdef __cplusplus
}
#endif