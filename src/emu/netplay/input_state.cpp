#include <vector>

#include "attotime.h"
#include "netplay.h"
#include "netplay/input_state.h"

netplay_analog_port& netplay_input_port::add_analog_port(int accum, int previous)
{
	m_analog_ports.emplace_back();
	auto& analog_port = m_analog_ports.back();
	analog_port.m_accum = accum;
	analog_port.m_previous = previous;
	return analog_port;
}

netplay_input_port& netplay_input::add_input_port(int digital)
{
	m_ports.emplace_back();
	auto& input_port = m_ports.back();
	input_port.m_digital = digital;
	return input_port;
}

std::string netplay_input::debug_string() const
{
	std::stringstream ss;

	ss << "input buffer\n";
	ss << "num_ports = " << m_ports.size() << "\n";

	for (auto i = 0; i < m_ports.size(); i++)
	{
		auto& port = m_ports[i];
		ss << "- port #" << i;
		ss << ", digital = " << port.m_digital << "\n";
		ss << "- num_analog = " << port.m_analog_ports.size() << "\n";

		for (auto q = 0; q < port.m_analog_ports.size(); q++)
		{
			auto& analog = port.m_analog_ports[i];
			ss << "- - analog #" << q << ", accum = " << analog.m_accum;
			ss << ", prev = " << analog.m_previous << "\n";
		}
	}

	return ss.str();
}
