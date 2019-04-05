#ifndef MAME_EMU_NETPLAY_MODULE_BLACKLIST_H
#define MAME_EMU_NETPLAY_MODULE_BLACKLIST_H

// this is a list of modules which are excluded from checksum computations
// usually because they produce non-deterministic results e.g. audio
// for performance reasons the module names are hashed to an unsigned int

#define BLACKLIST_MODULE(MODULE_HASH) case MODULE_HASH: return true

inline bool netplay_is_blacklisted(unsigned int module_hash)
{
  switch (module_hash)
  {
    BLACKLIST_MODULE(1389169213); // QSound (HLE)
  }

  return false;
}

#endif
