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

netplay_input::netplay_input(const attotime& timestamp, unsigned long long frame_index) :
	m_timestamp(timestamp), m_frame_index(frame_index) {}

netplay_input_port& netplay_input::add_input_port(int defvalue, int digital)
{
	m_ports.emplace_back();
	auto& input_port = m_ports.back();
	input_port.m_defvalue = defvalue;
	input_port.m_digital = digital;
	return input_port;
}
