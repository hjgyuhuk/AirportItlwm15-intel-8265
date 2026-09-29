#ifndef IWM_COMMAND_ID_H
#define IWM_COMMAND_ID_H

// Shared with transport tests; masked RX groups must be evaluated before shift.
#define IWM_WIDE_ID(grp, opcode) (((grp) << 8) | (opcode))

#endif
