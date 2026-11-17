// Diagnostic logging, enabled with [General] debug_log=true.
// Writes tcl-csvvw.log next to the DLL (or to %TEMP% if that folder is not writable).
#pragma once

namespace csvvw::dbg {

void configure(bool enabled);  // called on every file load
bool on();

void log(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace csvvw::dbg
