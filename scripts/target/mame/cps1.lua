-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   cps1.lua
--
--   Small driver-specific example makefile
--   Use make SUBTARGET=cps1 to build
--
---------------------------------------------------------------------------

--------------------------------------------------
-- Specify all the CPU cores necessary for the
-- drivers referenced in cps1.lst.
--------------------------------------------------

CPUS["Z80"] = true
CPUS["DSP16A"] = true
CPUS["M680X0"] = true

--------------------------------------------------
-- Specify all the sound cores necessary for the
-- drivers referenced in cps1.lst.
--------------------------------------------------

SOUNDS["YM2151"] = true
SOUNDS["YM2203"] = true
SOUNDS["OKIM6295"] = true
SOUNDS["MSM5205"] = true
SOUNDS["QSOUND"] = true

--------------------------------------------------
-- specify available video cores
--------------------------------------------------

--------------------------------------------------
-- specify available machine cores
--------------------------------------------------

MACHINES["TIMEKPR"] = true
MACHINES["EEPROMDEV"] = true
MACHINES["Z80CTC"] = true
MACHINES["I8255"] = true

--------------------------------------------------
-- specify available bus cores
--------------------------------------------------

--------------------------------------------------
-- This is the list of files that are necessary
-- for building all of the drivers referenced
-- in cps1.lst
--------------------------------------------------

function createProjects_mame_cps1(_target, _subtarget)
	project ("mame_cps1")
	targetsubdir(_target .."_" .. _subtarget)
	kind (LIBTYPE)
	uuid (os.uuid("drv-mame-cps1"))
	addprojectflags()
	precompiledheaders()

	includedirs {
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/devices",
		MAME_DIR .. "src/mame",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "3rdparty",
		GEN_DIR  .. "mame/layout",
	}

files{
	MAME_DIR .. "src/mame/drivers/cps1.cpp",
	MAME_DIR .. "src/mame/includes/cps1.h",
	MAME_DIR .. "src/mame/machine/kabuki.cpp",
	MAME_DIR .. "src/mame/video/cps1.cpp",
	MAME_DIR .. "src/mame/drivers/capcom.cpp",
	MAME_DIR .. "src/mame/includes/capcom.h",
	MAME_DIR .. "src/mame/drivers/fcrash.cpp",
	MAME_DIR .. "src/mame/drivers/fcrash.h",
	MAME_DIR .. "src/mame/drivers/kenseim.cpp",
	MAME_DIR .. "src/mame/drivers/kenseim.h"
}
end

function linkProjects_mame_cps1(_target, _subtarget)
	links {
		"mame_cps1",
	}
end
