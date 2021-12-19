#include "Config.h"
#include "Emulator.h"

#define VERTICAL_BLANK_SCAN_LINE 0x90
#define VERTICAL_BLANK_SCAN_LINE_MAX 0x99
#define RETRACE_START 456

Emulator::Emulator(void) :
  m_GameLoaded(false)
  ,m_CyclesThisUpdate(0)
  ,m_UsingMBC1(false)
  ,m_EnableRamBank(false)
  ,m_UsingMemoryModel16_8(true)
  ,m_EnableInterupts(false)
  ,m_PendingInteruptDisabled(false)
  ,m_PendingInteruptEnabled(false)
  ,m_RetraceLY(RETRACE_START)
  ,m_JoypadState(0)
  ,m_Halted(false)
  ,m_TimerVariable(0)
  ,m_CurrentClockSpeed(1024)
  ,m_DividerVariable(0)
  ,m_CurrentRamBank(0)
  ,m_DebugPause(false)
  ,m_DebugPausePending(false)
  ,m_TimeToPause(NULL)
  ,m_TotalOpcodes(0)
  ,m_DoLogging(false)
{
  ResetScreen( );
}

Emulator::~Emulator(void)
{
  for (std::vector<BYTE*>::iterator it = m_RamBank.begin(); it != m_RamBank.end(); it++)
    delete[] (*it);
}

bool Emulator::LoadRom(const std::string& romName)
{
  if (m_GameLoaded)
    StopGame( );

  m_GameLoaded = true;

  memset(m_Rom,0,sizeof(m_Rom));
  memset(m_GameBank,0,sizeof(m_GameBank));

  FILE *in;
  in = fopen( romName.c_str(), "rb" );
  fread(m_GameBank, 1, 0x200000, in);
  fclose(in);

  memcpy(&m_Rom[0x0], &m_GameBank[0], 0x8000); // this is read only and never changes

  m_CurrentRomBank = 1;

  return true;
}

bool Emulator::InitGame( RenderFunc func )
{
  m_RenderFunc = func;
  return ResetCPU( );
}

void Emulator::ResetScreen( )
{
  for (int x = 0; x < 144; ++x)
  {
    for (int y = 0; y < 160; ++y)
    {
      m_ScreenData[x][y][0] = 255;
      m_ScreenData[x][y][1] = 255;
      m_ScreenData[x][y][2] = 255;
    }
  }
}

bool Emulator::ResetCPU( )
{
  ResetScreen( );
  m_DoLogging = false;
  m_CurrentRamBank = 0;
  m_TimerVariable = 0;
  m_CurrentClockSpeed = 1024;
  m_DividerVariable = 0;
  m_Halted = false;
  m_TotalOpcodes = 0;
  m_JoypadState = 0xFF;
  m_CyclesThisUpdate = 0;
  m_ProgramCounter = 0x100;
  m_RegisterAF.hi = 0x1;
  m_RegisterAF.lo = 0xB0;
  m_RegisterBC.reg = 0x0013;
  m_RegisterDE.reg = 0x00D8;
  m_RegisterHL.reg = 0x014D;
  m_StackPointer.reg = 0xFFFE;

  m_Rom[0xFF00] = 0xFF;
  m_Rom[0xFF05] = 0x00;
  m_Rom[0xFF06] = 0x00;
  m_Rom[0xFF07] = 0x00;
  m_Rom[0xFF10] = 0x80;
  m_Rom[0xFF11] = 0xBF;
  m_Rom[0xFF12] = 0xF3;
  m_Rom[0xFF14] = 0xBF;
  m_Rom[0xFF16] = 0x3F;
  m_Rom[0xFF17] = 0x00;
  m_Rom[0xFF19] = 0xBF;
  m_Rom[0xFF1A] = 0x7F;
  m_Rom[0xFF1B] = 0xFF;
  m_Rom[0xFF1C] = 0x9F;
  m_Rom[0xFF1E] = 0xBF;
  m_Rom[0xFF20] = 0xFF;
  m_Rom[0xFF21] = 0x00;
  m_Rom[0xFF22] = 0x00;
  m_Rom[0xFF23] = 0xBF;
  m_Rom[0xFF24] = 0x77;
  m_Rom[0xFF25] = 0xF3;
  m_Rom[0xFF26] = 0xF1;
  m_Rom[0xFF40] = 0x91;
  m_Rom[0xFF42] = 0x00;
  m_Rom[0xFF43] = 0x00;
  m_Rom[0xFF45] = 0x00;
  m_Rom[0xFF47] = 0xFC;
  m_Rom[0xFF48] = 0xFF;
  m_Rom[0xFF49] = 0xFF;
  m_Rom[0xFF4A] = 0x00;
  m_Rom[0xFF4B] = 0x00;
  m_Rom[0xFFFF] = 0x00;
  m_RetraceLY = RETRACE_START;

  m_DebugValue = m_Rom[0x40];

  m_EnableRamBank = false;

  m_UsingMBC2 = false;

  // what kinda rom switching are we using, if any?
  switch(ReadMemory(0x147))
  {
    case 0: m_UsingMBC1 = false; break; // not using any memory swapping
    case 1:   
    case 2:
    case 3 : m_UsingMBC1 = true; break;
    case 5 : m_UsingMBC2 = true; break;
    case 6 : m_UsingMBC2 = true; break;
    default: return false; // unhandled memory swapping, probably MBC2
  }
  
  // how many ram banks do we need, if any?
  int numRamBanks = 0;
  switch (ReadMemory(0x149))
  {
    case 0: numRamBanks = 0; break;
    case 1: numRamBanks = 1; break;
    case 2: numRamBanks = 1; break;
    case 3: numRamBanks = 4; break;
    case 4: numRamBanks = 16; break;
  }

  CreateRamBanks(numRamBanks);
  return true;
}

static int hack = 0;
static long long counter9 = 0;

// remember this update function is not the same as the virtual update function. This gets specifically 
// called by the Game::Update function. This way I have control over when to exectue the next opcode. Mainly
// for the debug window.
void Emulator::Update( )
{
  hack++;

  m_CyclesThisUpdate = 0;
  const int m_TargetCycles = 70221;

  while ((m_CyclesThisUpdate < m_TargetCycles)) //||(ReadMemory(0xFF44) < 144))
  {
    if (m_DebugPause)
      return;
    if (m_DebugPausePending)
    {
      if (m_TimeToPause && (m_TimeToPause() == true))
      {
        m_DebugPausePending = false;
        m_DebugPause = true;
        return;
      }
    }
    
    int currentCycle = m_CyclesThisUpdate;
    BYTE opcode = ExecuteNextOpcode( );
    int cycles = m_CyclesThisUpdate - currentCycle;

    DoTimers( cycles );
    DoGraphics( cycles );
    DoInput( );
    DoInterupts( );
  }

  counter9 += m_CyclesThisUpdate;
  m_RenderFunc( );
}

void Emulator::DoInput( )
{

}

void Emulator::DoGraphics( int cycles )
{
  SetLCDStatus( );

  // count down the LY register which is the current line being drawn. When reaches 144 (0x90) its vertical blank time
  if (TestBit(ReadMemory(0xFF40), 7))
    m_RetraceLY -= cycles;

  if (m_Rom[0xFF44] > VERTICAL_BLANK_SCAN_LINE_MAX)
    m_Rom[0xFF44] = 0;
  //else if (m_Rom[0xFF44] == 0)
  // ResetScreen( );

  if (m_RetraceLY <= 0)
    DrawCurrentLine( );
}

BYTE Emulator::ExecuteNextOpcode( )
{
  BYTE opcode = m_Rom[m_ProgramCounter];

  if ((m_ProgramCounter >= 0x4000 && m_ProgramCounter <= 0x7FFF) || (m_ProgramCounter >= 0xA000 && m_ProgramCounter <= 0xBFFF))
    opcode = ReadMemory(m_ProgramCounter);

  if (!m_Halted)
  {
    if (false)
    {
      char buffer[200];
      sprintf(buffer, "OP = %x PC = %\n", opcode, m_ProgramCounter);
      LogMessage::GetSingleton()->DoLogMessage(buffer,false);
    }

    m_ProgramCounter++;
    m_TotalOpcodes++;

    ExecuteOpcode( opcode );
  }
  else
  {
    m_CyclesThisUpdate += 4;
  }

  // we are trying to disable interupts, however interupts get disabled after the next instruction
  // 0xF3 is the opcode for disabling interupt
  if (m_PendingInteruptDisabled)
  {
    if (ReadMemory(m_ProgramCounter-1) != 0xF3)
    {
      m_PendingInteruptDisabled = false;
      m_EnableInterupts = false;
    }
  }

  if (m_PendingInteruptEnabled)
  {
    if (ReadMemory(m_ProgramCounter-1) != 0xFB)
    {
      m_PendingInteruptEnabled = false;
      m_EnableInterupts = true;
    }
  }

  return opcode;
}

void Emulator::StopGame( )
{
  m_GameLoaded = false;
}

std::string Emulator::GetCurrentOpcode( ) const
{
  return std::string("%x", m_Rom[m_ProgramCounter]);
}

std::string Emulator::GetImmediateData1( ) const
{
  return std::string("%x", m_Rom[m_ProgramCounter+1]);
}

std::string Emulator::GetImmediateData2( ) const
{
  return std::string("%x", m_Rom[m_ProgramCounter+2]);
}

// all reading of rom should go through here so I can trap it
BYTE Emulator::ReadMemory(WORD memory) const
{
  // reading from rom bank
  if (memory >= 0x4000 && memory <= 0x7FFF)
  {
    unsigned int newAddress = memory;
    newAddress += ((m_CurrentRomBank-1)*0x4000;
    return m_GameBank[newAddress];
  }
  
  // reading from RAM bank
  else if (memory >= 0xA000 && memory <= 0xBFFF)
  {
    WORD newAddress = memory - 0xA000;
    return m_RamBank.at(m_CurrentRamBank)[newAddress];
  }
  // trying to read joypad state
  else if (memory == 0xFF00)
    return GetJoypadState( );

  return m_Rom[memory];
}

WORD Emulator::ReadWord( ) const
{
  WORD res = ReadMemory(m_ProgramCounter+1);
  res = res << 8;
  res |= ReadMemory(m_ProgramCounter);
  return res;
}

WORD Emulator::ReadLSWord( ) const
{
  WORD res = ReadMemory(m_ProgramCounter+1);
  res = res << 8;
  res |= ReadMemory(m_ProgramCounter);
  return res;
}

// writes a byte to memory. Remember that address 0 - 07FFF is rom so we can't write to this address
void Emulator::WriteByte(WORD address, BYTE data)
{
  // writing to memory address 0x0 to 0x1FFF this disables writing to the ram bank. 0 disables, 0xA enables
  if (address <= 0x1FFF)
  {
    if (m_UsingMBC1)
    {
      if ((data & 0xF) == 0xA)
        m_EnableRamBank = true;
      else if (data == 0x0)
        m_EnableRamBank = false;
    }
    else if (m_UsingMBC2)
    {
      // bit 0 of upper byte must be 0
      if (false == TestBit(address, 8))
      {
        if ((data & 0xF) == 0xA)
          m_EnableRamBank = true;
        else if (data == 0x0)
          m_EnableRamBank = false;
      }
    }
  }
  
  // if writing to a memory address between 2000 and 3FFF then we need to change rom bank
  else if ((address >= 0x2000) && (address <= 0x3FFF))
  {
    if (m_UsingMBC1)
    {
      if (data == 0x00)
        data++;

      data &= 31;

      // Turn off the lower 5 bits
      m_CurrentRomBank &= 224;

      // Combine the written data with the register
      m_CurrentRomBank |= data;

      char buffer[256];
      sprint(buffer, "Changing Rom Bank to %d", m_CurrentRomBank);
      LogMessage::GetSingleton()->DoLogMessage(buffer, false);
    }
    else if (m_UsingMBC2)
    {
      data &= 0xF;
      m_CurrentRomBank = data;
    }
  }

  // writing to address 0x4000 to 0x5FFF switches ram banks (if enabled of course)
  else if ((address >= 0x4000) && (address <= 0x5FFF))
  {
    if (m_UsingMBC1)
    {
      // are we using memory model 16/8
      if (m_UsingMemoryModel16_8)
      {
        // in this mode we can only use Ram Bank 0
        m_CurrentRamBank = 0;

        data &= 3;
        data <<= 5;

        if ((m_CurrentRomBank & 31) == 0)
        {
          data++;
        }

        // Turn off bits 5 and 6, and 7 if it somehow got turned on
        m_CurrentRomBank &= 31;
        
        // Combine the written data with the register
        m_CurrentRomBank |= data;

        char buffer[256];
        sprintf(buffer, "Changing Rom Bank to %d", m_CurrentRomBank);
        LogMessage::GetSingleton()->DoLogMessage(buffer, false);
      }
      else
      {
        m_CurrentRomBank = data & 0x3;
        char buffer[256];
        sprintf(buffer, "Changing Rom Bank to %d", m_CurrentRomBank);
        LogMessage::GetSingleton()->DoLogMessage(buffer, false);
      }
    }
  }

  // writing to address 0x6000 to 0x7FFF switches memory model
  else if ((address >= 0x6000) && (address <= 0x7FFF))
  {
    if (m_UsingMBC1)
    {
      // we're only interested in the first bit
      data &= 1;
      if (data == 1)
      {
        m_CurrentRamBank = 0;
        m_UsingMemoryModel16_8 = false;
      }
      else 
      {
        m_UsingMemoryModel16_8 = true;
      }
    }
  }  

  // fron now on we're writing to RAM
  else if ((address >= 0xA000) && (address <= 0xBFFF))
  {
    if (m_EnableRamBank)
    {
      if (m_UsingMBC1)
      {
        WORD newAddress = address - 0xA000;
        m_RamBank.at(m_CurrentRamBank)[newAddress] = data;
      }
    }
    else if (m_UsingMBC2 && (address < 0xA200))
    { 
      WORD newAddress = address - 0xA000;
      m_RamBank.at(m_CurrentRamBank)[newAddress] = data;
    }
  }

  // we're right to internal RAM, remember that it needs to echo it
  else if ((address >= 0xC000) && (address < 0xDFFF))
  {
    m_Rom[address] = data;
  }

  // echo memory. Writes here and into the internal ram. Same as above
  else if ((address >= 0xE000) && (address <= 0xFDFF))
  {
    m_Rom[address] = data;
    m_Rom[address -0x2000] = data; // echo data into ram address
  }

  // this area is restricted
  else if ((address >= 0xFEA0) && (address <= 0xFEFF))
  {
  }

  // reset the divider register
  else if (address == 0xFF04)
  {
    m_Rom[0xFF04] = 0;
    m_DividerVariable = 0;
  }

  // not sure if this is correct
  else if (address == 0xFF07)
  {
    m_Rom[address] = data;
   
    int timerVal = data & 0x03;

    int clockSpeed = 0;

    switch(timerVal)
    {
      case 0: clockSpeed = 1024; break;
      case 1: clockSpeed = 16; break;
      case 2: clockSpeed = 64; break;
      case 3: clockSpeed = 256; break; // 256
      default: assert(false); break; // weird timer val
    }

    if (clockSpeed != m_CurrentClockSpeed)
    {
      m_TimerVariable = 0;
      m_CurrentClockSpeed = clockSpeed;
    }
  }

  // FF44 shows which horizontal scanline is currently being drawn. Writing here resets it
  else if (address == 0xFF44)
  {
    m_Rom[0xFF44] = 0;
  }

  else if (address == 0xFF45)
  {
    m_Rom[address] = data;
  }

  // DMA transfer
  else if (address == 0xFF46)
  {
    WORD newAddress = (data << 8);
    for (int i = 0; i < 0xA0; ++i)
    {
      m_Rom[0xFE00 + i] = ReadMemory(newAddress + i);
    }
  }

  // This area is restricted
  else if ((address >= 0xFF4C) && (address <= 0xFF7F))
  {
  }

  // I guess we're ok to write to memory... gulp
  else 
  {
    m_Rom[address] = data;
  }
}

int Emulator::GetCarryFlag() const
{
  if (TestBit(m_RegisterAF.lo, FLAG_C))
    return 1;

  return 0;
}

int Emulator::GetZeroFlag() const
{
  if (TestBit(m_RegisterAF.lo, FLAG_Z))
    return 1;

  return 0;
}

int Emulator::GetHalfCarryFlag() const
{
  if (TestBit(m_RegisterAF.lo, FLAG_H))
    return 1;

  return 0;
}

int Emulator::GetSubtractFlag() const
{
  if (TestBit(m_RegisterAF.lo, FLAG_N))
    return 1;

  return 0;
}

static int vblankcount = 0;

void Emulator::IssueVerticalBlank()
{
  vblankcount++;
  RequestInterupt(0);
  if (hack == 60)
  {
    //OutputDebugStr(STR::Format("Total VBlanks was: %d\n", vblankcount));
    vblankcount = 0;
  }
}

static int counter = 0;
static int count2 = 0;

void Emulator::DrawCurrentLine()
{
  if (TestBit(ReadMemory(0xFF40), 7) == false)
    return;

  m_Rom[0xFF44]++;
  m_RetraceLY = RETRACE_START;

  BYTE scanLine = ReadMemory(0xFF44);

  if (scanLine == VERTICAL_BLANK_SCAN_LINE)
    IssueVerticalBlank();

  if (scanLine > VERTICAL_BLANK_SCAN_LINE_MAX)
    m_Rom[0xFF44] = 0;   

  if (scanLine < VERTICAL_BLANK_SCAN_LINE)
  {
    DrawScanLine();
  }
}

void Emulator::PushWordOntoStack(WORD word)
{
  BYTE hi = word >> 8;
  BYTE lo = word & 0xFF;
  m_StackPointer.reg--;
  WriteByte(m_StackPointer.reg, hi);
  m_StackPointer.reg--;
  WriteByte(m_StackPointer.reg, lo);
}

WORD Emulator::PopWordOffStack()
{
  WORD word = ReadMemory(m_StackPointer.reg+1) << 8;
  word |= ReadMemory(m_StackPointer.reg);
  m_StackPointer.reg+=2;

  return word;
}

void Emulator::DoInterupts()
{
  // are interupts enabled
  if (m_EnableInterupts)
  {
    // has anything requested an interrupt?
    BYTE requestFlag = ReadMemory(0xFF0F);
    if (requestFlag > 0)
    {
      // which requested interrupt has the lowest priority?
      for (int bit = 0; bit < 8; ++bit)
      {
        if (TestBit(requestFlag, bit))
        {
          // this interrupt has been requested. But is it enabled?
          BYTE enabledReg = ReadMemory(0xFFFF);
          if (TestBit(enabledReg, bit))
          {
            // yup it is enabled, so lets DOOOO ITTTTT
            ServiceInterrupt(bit);
          }
        }
      }
    }
  }
}

void Emulator::ServiceInterrupt( int num )
{
  // save current program counter
  PushWordOntoStack(m_ProgramCounter);
  m_Halted = false;

  char buffer[200];
  sprintf(buffer, "servicing interrupt %d", num);
  LogMessage::GetSingleton()->DoLogMessage(buffer, false);

  // unsigned long long limit = (8000000);
  // if (m_TotalOpcodes > limit)
  // LOGMESSAGE(Logging::MSG_INFO, STR::Format("Servicing interrupt %d", num).ConstCharPtr());

  switch(num)
  {
    case 0: m_ProgramCounter = 0x40; break; // V-Blank
    case 1: m_ProgramCounter = 0x48; break; // LCD-STATE
    case 2: m_ProgramCounter = 0x50; break; // Timer
    case 4: m_ProgramCounter = 0x60; break; // JoyPad
    default: assert(false); break;
  }

  m_EnableInterrupts = false;
  m_Rom[0xFF0F] = BitReset(m_Rom[0xFF0F], num);
}

void Emulator::DrawScanLnie()
{
  BYTE lcdControl = ReadMemory(0xFF40);

  // we can only draw if the LCD is enabled
  if (TestBit(lcdControl, 7))
  {
    RenderBackground( lcdControl );
    RenderSprites( lcdControl );
    // m_RenderFunc();
  } 
}

void Emulator::RenderBackgrond(BYTE lcdControl)
{
  // lets draw the background (however it does need to be enabled)
  if (TestBit(lcdControl, 0))
  {
    WORD tileData = 0;
    WORD backgroundMemory = 0;
    bool unsig = true;

    BYTE scrollY = ReadMemory(0xFF42);
    BYTE scrollX = ReadMemory(0xFF43);
    BYTE windowY = ReadMemory(0xFF4A);
    BYTE windowX = ReadMemory(0xFF4B) - 7;

    bool usingWindow = false;

    if (TestBit(lcdControl, 5))
    {
      if (windowY <= ReadMemory(0xFF44))
        usingWindow = true;
    }
    else 
    {
      usingWindow = false;
    }

    // which tile data are we using?
     if (TestBit(lcdControl, 4))
    {
      tileData = 0x8000;
    }
    else 
    {
      tileData = 0x8800;
      unsig = false;
    }

    // which background mem?
    if (false == usingWindow)
    {
      if (TestBit(lcdControl, 3))
        backgroundMemory = 0x9C00;
      else 
        backgroundMemory = 0x9800;
    }
    else
    {
      if (TestBit(lcdControl, 6))
        backgroundMemory = 0x9C00;
      else 
        backgroundMemory = 0x9800;
    }  

    BYTE yPos = 0;

    if (!usingWindow)
      yPos = scrollY + ReadMemory(0xFF44);
    else
      yPos = ReadMemory(0xFF44) - windowY;

    WORD tileRow = (((BYTE)(yPos/8))*32);

    for (int pixel = 0; pixel < 160; ++pixel)
    {
      BYTE xPos = pixel+scrollX;

      if (usingWindow)
      {
        if (pixel >= windowX)
        {
          xPos = pixel - windowX;
        }
      }

      WORD tileCol = (xPos/8);
      SIGNED_WORD tileNum;

      if(unsig)
        tileNum = (BYTE)ReadMemory(backgroundMemory+tileRow + tileCol);
      else
        tileNum = (SIGNED_BYTE)ReadMemory(backgroundMemory+tileRow + tileCol);
 
      WORD tileLocation = tileData;

      if (unsig)
        tileLocation += (tileNum * 16);
      else
        tileLocation += ((tileNum+128) * 16);

      

    }

  }

}
