// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, Brad Hughes
//============================================================
//
//  input_sdlcommon.cpp - SDL Common code shared by SDL modules
//
//    Note: this code is also used by the X11 input modules
//
//============================================================

#include "input_module.h"
#include "modules/osdmodule.h"

#if defined(OSD_SDL)

// standard sdl header
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stddef.h>
#include <mutex>
#include <memory>
#include <algorithm>

// MAME headers
#include "emu.h"
#include "osdepend.h"
#include "ui/uimain.h"
#include "uiinput.h"
#include "window.h"
#include "strconv.h"

#include "../../sdl/osdsdl.h"
#include "input_common.h"
#include "input_sdlcommon.h"

#define GET_WINDOW(ev) window_from_id((ev)->windowID)
//#define GET_WINDOW(ev) ((ev)->windowID)

static std::shared_ptr<sdl_window_info> window_from_id(Uint32 windowID)
{
	SDL_Window *sdl_window = SDL_GetWindowFromID(windowID);

	auto& windows = osd_common_t::s_window_list;
	auto window = std::find_if(windows.begin(), windows.end(), [sdl_window](std::shared_ptr<osd_window> w)
	{
		return std::static_pointer_cast<sdl_window_info>(w)->platform_window() == sdl_window;
	});

	if (window == windows.end())
		return nullptr;

	return std::static_pointer_cast<sdl_window_info>(*window);
}

void sdl_event_manager::process_events(running_machine &machine)
{
	std::lock_guard<std::mutex> scope_lock(m_lock);
	SDL_Event sdlevent;
	while (SDL_PollEvent(&sdlevent))
	{
		// process window events if they come in
		if (sdlevent.type == SDL_WINDOWEVENT)
			process_window_event(machine, sdlevent);

		// Find all subscribers for the event type
		auto subscribers = m_subscription_index.equal_range(sdlevent.type);

		// Dispatch the events
		std::for_each(subscribers.first, subscribers.second, [&sdlevent](auto sub)
		{
			sub.second->handle_event(sdlevent);
		});
	}
}

void sdl_event_manager::process_window_event(running_machine &machine, SDL_Event &sdlevent)
{
	std::shared_ptr<sdl_window_info> window = GET_WINDOW(&sdlevent.window);

	if (window == nullptr)
	{
		// This condition may occur when the fullscreen toggle is used
		osd_printf_verbose("Skipped window event due to missing window param from SDL\n");
		return;
	}

	switch (sdlevent.window.event)
	{
	case SDL_WINDOWEVENT_SHOWN:
		m_has_focus = true;
		break;

	case SDL_WINDOWEVENT_CLOSE:
		machine.schedule_exit();
		break;

	case SDL_WINDOWEVENT_LEAVE:
		machine.ui_input().push_mouse_leave_event(window->target());
		m_mouse_over_window = 0;
		break;

	case SDL_WINDOWEVENT_MOVED:
		window->notify_changed();
		m_focus_window = window;
		m_has_focus = true;
		break;

	case SDL_WINDOWEVENT_RESIZED:
#ifdef SDLMAME_LINUX
		/* FIXME: SDL2 sends some spurious resize events on Ubuntu
		* while in fullscreen mode. Ignore them for now.
		*/
		if (!window->fullscreen())
#endif
		{
			//printf("event data1,data2 %d x %d %ld\n", event.window.data1, event.window.data2, sizeof(SDL_Event));
			window->resize(sdlevent.window.data1, sdlevent.window.data2);
		}
		m_focus_window = window;
		m_has_focus = true;
		break;

	case SDL_WINDOWEVENT_ENTER:
		m_mouse_over_window = 1;
		/* fall through */
	case SDL_WINDOWEVENT_FOCUS_GAINED:
	case SDL_WINDOWEVENT_EXPOSED:
	case SDL_WINDOWEVENT_MAXIMIZED:
	case SDL_WINDOWEVENT_RESTORED:
		m_focus_window = window;
		m_has_focus = true;
		break;

	case SDL_WINDOWEVENT_MINIMIZED:
	case SDL_WINDOWEVENT_FOCUS_LOST:
		m_has_focus = false;
		break;
	}
}

//============================================================
//  customize_input_type_list
//============================================================

void sdl_osd_interface::customize_input_type_list(simple_list<input_type_entry> &typelist)
{
}

void sdl_osd_interface::poll_inputs(running_machine &machine)
{
	m_keyboard_input->poll_if_necessary(machine);
	m_mouse_input->poll_if_necessary(machine);
	m_lightgun_input->poll_if_necessary(machine);
	m_joystick_input->poll_if_necessary(machine);
}

void sdl_osd_interface::release_keys()
{
	auto keybd = dynamic_cast<input_module_base*>(m_keyboard_input);
	if (keybd != nullptr)
		keybd->devicelist()->reset_devices();
}

bool sdl_osd_interface::should_hide_mouse()
{
	// if we are paused, no
	if (machine().paused())
		return false;

	// if neither mice nor lightguns enabled in the core, then no
	if (!options().mouse() && !options().lightgun())
		return false;

	if (!sdl_event_manager::instance().mouse_over_window())
		return false;

	// otherwise, yes
	return true;
}

void sdl_osd_interface::process_events_buf()
{
	SDL_PumpEvents();
}

#endif
