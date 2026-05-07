#include "MicroPythonMatrixService.h"

#include <Arduino.h>

extern "C" {
#include "matrix_app_api.h"
}

void MicroPythonMatrixService::service() {
  matrix_app_service_tick(millis());
}

const char* MicroPythonMatrixService::name() const {
  return "MPMatrixApp";
}

void MicroPythonMatrixService::beginOverride() {
  matrix_app_begin_override();
}

void MicroPythonMatrixService::endOverride() {
  matrix_app_end_override();
}

bool MicroPythonMatrixService::isOverridden() const {
  return matrix_app_is_overridden() != 0;
}

bool MicroPythonMatrixService::hasCallback() const {
  return matrix_app_is_active() != 0;
}
