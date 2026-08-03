#ifndef COREKILN_SYSTEM_THERMAL_H
#define COREKILN_SYSTEM_THERMAL_H

typedef enum {
  COREKILN_THERMAL_UNKNOWN = 0,
  COREKILN_THERMAL_NOMINAL,
  COREKILN_THERMAL_FAIR,
  COREKILN_THERMAL_SERIOUS,
  COREKILN_THERMAL_CRITICAL,
} system_thermal_state;

system_thermal_state system_thermal_current(void);
const char *system_thermal_name(system_thermal_state state);

#endif
