#include <vector>
#include <string>

#include "attotime.h"
#include "netplay.h"
#include "netplay/input_state.h"

std::string netplay_input::debug_string() const
{
	std::stringstream ss;

	ss << "input buffer\n";
	ss << "frame_index = " << m_frame_index << "\n";
	ss << "num_ports = " << m_ports.size() << "\n";

	for (auto i = 0; i < m_ports.size(); i++)
	{
		auto& port = m_ports[i];
		ss << "- port #" << i;
		ss << ", digital = " << port.m_digital << "\n";
		ss << "- num_analog = " << port.m_analog.size() << "\n";

		for (auto q = 0; q < port.m_analog.size(); q++)
		{
			auto& analog = port.m_analog[i];
			ss << "- - analog #" << q << ", accum = " << analog.m_accum;
			ss << ", prev = " << analog.m_previous << "\n";
		}
	}

	return ss.str();
}
