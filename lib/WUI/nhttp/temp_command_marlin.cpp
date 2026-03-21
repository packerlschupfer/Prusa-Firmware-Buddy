#include "temp_command.h"

#include <marlin_client.hpp>
#include <cstdio>
#include "printers.h"

#if PRINTER_IS_PRUSA_COREONE()
    #include <feature/chamber/chamber.hpp>
#endif

namespace nhttp::printer {

bool TempCommand::set_nozzle_temp(int temp) {
    char gcode[20];
    snprintf(gcode, sizeof(gcode), "M104 S%d", temp);
    marlin_client::gcode(gcode);
    return true;
}

bool TempCommand::set_bed_temp(int temp) {
    char gcode[20];
    snprintf(gcode, sizeof(gcode), "M140 S%d", temp);
    marlin_client::gcode(gcode);
    return true;
}

bool TempCommand::set_chamber_temp(int temp) {
#if PRINTER_IS_PRUSA_COREONE()
    if (temp <= 0) {
        buddy::chamber().set_target_temperature(std::nullopt);
    } else {
        buddy::chamber().set_target_temperature(temp);
    }
    return true;
#else
    (void)temp;
    return false;
#endif
}

} // namespace nhttp::printer
