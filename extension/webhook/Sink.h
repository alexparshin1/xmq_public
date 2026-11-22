#pragma once

#include "extension/xmq_extension.h"
#include <cstddef>

// Versioned independently from the extension discovery descriptor. Each worker owns one
// instance. Strings passed across this boundary are borrowed only for the duration of the call.
struct XmqSinkV1
{
    using Setting = const char* (*)(void*, const char*);
    void* (*create)(Setting, void*, char*, size_t);
    void (*destroy)(void*);
    // 0: delivered, 1: permanent failure, 2: reconnect and retry.
    int (*deliver)(void*, const char*, size_t, const char* const*, const char* const*, size_t);
};

using XmqSinkDescribeV1 = const XmqSinkV1* (*)();
