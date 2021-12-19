#include <string.h>

// returns if a bit set
template <typename t>
bool TestBit( t data, int position )
{
  t mask = 1 << position;
  return ( data & mask ) ? true : false;
}

template <typename t>
bool BitSet( t data, int position )
{
  t mask = 1 << position;
  data |= mask;
  return data;
}

template <typename t>
bool BitReset( t data, int position )
{
  t mask = 1 << position;
  data &= ~mask;
  return data;
}

template <typename t>
bool BitGetVal( t data, int position )
{
  t mask = 1 << position;
  return ( data & mask ) ? 1 : 0;
}
