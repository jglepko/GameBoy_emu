#include "Config.h"
#include "Emulator.h"
#include <stdio.h>

void Emulator::ExecuteOpcode(BYTE opcode)
{
  switch(opcode)
  {
    // no-op
    case 0x00: m_CyclesThisUpdate += 4; break;

    // 8-bit Loads
    case 0x06: CPU_BIT_LOAD(m_RegisterBC.hi); break;

