#import "system_thermal.h"

#import <Foundation/Foundation.h>

system_thermal_state system_thermal_current(void) {
  switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal:
      return COREKILN_THERMAL_NOMINAL;
    case NSProcessInfoThermalStateFair:
      return COREKILN_THERMAL_FAIR;
    case NSProcessInfoThermalStateSerious:
      return COREKILN_THERMAL_SERIOUS;
    case NSProcessInfoThermalStateCritical:
      return COREKILN_THERMAL_CRITICAL;
  }
  return COREKILN_THERMAL_UNKNOWN;
}

const char *system_thermal_name(system_thermal_state state) {
  switch (state) {
    case COREKILN_THERMAL_NOMINAL:
      return "nominal";
    case COREKILN_THERMAL_FAIR:
      return "fair";
    case COREKILN_THERMAL_SERIOUS:
      return "serious";
    case COREKILN_THERMAL_CRITICAL:
      return "critical";
    case COREKILN_THERMAL_UNKNOWN:
      return "unknown";
  }
  return "unknown";
}
