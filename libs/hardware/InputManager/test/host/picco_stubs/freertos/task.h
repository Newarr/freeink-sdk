#pragma once
#include <cstdint>

#include "FreeRTOS.h"
inline void vTaskDelay(int) {}
inline int xTaskCreate(void (*)(void*),const char*,int,void*,int,void**) { return 0; }
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, UBaseType_t,
                                        TaskHandle_t*, BaseType_t) {
  return 0;
}
