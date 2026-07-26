// Render `abi::ProgramFacts` as the `cel inspect` report.
//
// Presentation only — the facts themselves come from
// `//abi:program_facts`, which any embedder can use.  This is the part
// that is genuinely CLI-specific: label widths, `none` sentinels, and
// the runnability warning a terminal user needs.

#ifndef CELWASM_TOOLS_CEL_PROGRAM_REPORT_H_
#define CELWASM_TOOLS_CEL_PROGRAM_REPORT_H_

#include <string>

#include "abi/program_facts.h"

namespace celwasm::tools::cel {

std::string FormatProgramFacts(const ::celwasm::abi::ProgramFacts& facts);

}  // namespace celwasm::tools::cel

#endif  // CELWASM_TOOLS_CEL_PROGRAM_REPORT_H_
