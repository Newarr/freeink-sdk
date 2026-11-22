#pragma once
using BaseType_t = int;
using UBaseType_t = unsigned int;
using QueueHandle_t=void*;
using TaskHandle_t=void*;
constexpr int pdTRUE=1;
constexpr int pdPASS=1;
constexpr BaseType_t tskNO_AFFINITY = -1;
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
#define pdMS_TO_TICKS(x) (x)
