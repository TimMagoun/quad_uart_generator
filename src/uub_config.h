#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef UUB_ENABLE_DEBUG_CDC
#define UUB_ENABLE_DEBUG_CDC 0
#endif

#ifndef UUB_BRIDGE_COUNT
#define UUB_BRIDGE_COUNT 6
#endif

namespace uub {

constexpr size_t kBridgeCount = UUB_BRIDGE_COUNT;
constexpr size_t kReadBufferSize = 256;
constexpr uint32_t kDefaultUartBaud = 115200;
constexpr int kUartToUsbBatch = 28;
constexpr int kUsbToUartBudget = 28;
constexpr uint32_t kLineCodingSettleMs = 25;

}  // namespace uub
