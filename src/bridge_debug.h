#pragma once

#include <stdint.h>

#include <tusb.h>

namespace uub::debug {

void begin();
void logConfigResult(const char *description,
                     bool applied,
                     uint32_t requestedBaud,
                     uint16_t requestedConfig,
                     uint32_t activeBaud,
                     uint16_t activeConfig);
void logLineCodingSkip(uint8_t itf);
void logLineCodingApply(uint8_t itf,
                        uint8_t bridgeIndex,
                        const char *description,
                        cdc_line_coding_t const *lineCoding);
void logPioRestartStop();
void logPioRestartStart();

}  // namespace uub::debug
