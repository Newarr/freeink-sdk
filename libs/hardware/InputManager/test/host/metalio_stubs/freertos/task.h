#pragma once
#include <cstdint>
#include <cstring>

#include "FreeRTOS.h"
inline void vTaskDelay(int) {}
inline int xTaskCreate(void (*)(void*),const char*,int,void*,int,void**) { return 0; }
// Runs the task body to completion before returning, so host tests see a
// background start finish deterministically.
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, uint32_t, void* arg,
                                        UBaseType_t, TaskHandle_t*, BaseType_t) {
  if (std::strcmp(name, "gsl_init") != 0) return 0;
  fn(arg);
  return pdPASS;
}
inline void vTaskDelete(void*) {}
