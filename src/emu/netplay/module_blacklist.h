#ifndef MAME_EMU_NETPLAY_MODULE_BLACKLIST_H
#define MAME_EMU_NETPLAY_MODULE_BLACKLIST_H

#include "util/hash.h"

// this is a list of modules which are excluded from checksum computations
// usually because they produce non-deterministic results e.g. audio

#define MODULE(MODULE_HASH) case MODULE_HASH: return true;

inline bool netplay_is_blacklisted(unsigned int module_hash)
{
  switch (module_hash)
  {
    MODULE(0x03962e98) // QSound (HLE)
  }

  return false;
}

#endif
