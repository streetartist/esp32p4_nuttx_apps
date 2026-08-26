#pragma once

#define FE_TONEAREST  0x00000000
#define FE_TOWARDZERO 0x00000001
#define FE_DOWNWARD   0x00000002
#define FE_UPWARD     0x00000003

int fesetround(int mode);
