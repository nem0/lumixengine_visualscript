#include "m3_lumix.h"
#include "../external/m3_env.h"

int m3l_getGlobalCount(IM3Module module) {
	return module->numGlobals;
}

const char* m3l_getGlobalName(IM3Module module, int idx) {
	return module->globals[idx].name;
}
