// SimpleMath outside Windows, in the browser.
//
// SimpleMath converts its rectangles and viewports to and from the Windows
// RECT, LONG and UINT types. Outside Windows they are defined here.

#pragma once

#ifndef _WIN32
typedef unsigned int UINT;
typedef long LONG;
typedef struct tagRECT
{
    long left;
    long top;
    long right;
    long bottom;
} RECT;
#endif

#include "DirectXMath.h"
#include_next <SimpleMath.h>
