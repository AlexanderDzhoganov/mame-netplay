#include <vector>

#include "emu.h"
#include "netplay_input.h"

netplay_analog_port& netplay_input_port::add_analog_port(int accum, int previous, int sensitivity, int reverse)
{
	m_analog_ports.emplace_back();
	auto& analog_port = m_analog_ports.back();
	analog_port.m_accum = accum;
	analog_port.m_previous = previous;
	analog_port.m_sensitivity = sensitivity;
	analog_port.m_reverse = reverse;
	return analog_port;
}

netplay_input::netplay_input(const attotime& timestamp) :
	m_timestamp(timestamp), m_consumed(false) {}

netplay_input_port& netplay_input::add_input_port(int defvalue, int digital)
{
	m_ports.emplace_back();
	auto& input_port = m_ports.back();
	input_port.m_defvalue = defvalue;
	input_port.m_digital = digital;
	return input_port;
}

// returns when this input state is supposed to take effect
attotime netplay_input::calculate_future_time(int latency_ms) const
{
	latency_ms = std::max(40, std::min(500, latency_ms));
	return m_timestamp + attotime(0, ATTOSECONDS_PER_MILLISECOND * latency_ms);
}
