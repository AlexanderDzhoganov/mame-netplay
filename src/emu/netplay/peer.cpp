#include <string>
#include <memory>

#include "netplay.h"
#include "netplay/util.h"
#include "netplay/input_state.h"
#include "netplay/peer.h"

netplay_peer::netplay_peer(const std::string& name, const netplay_addr& address, attotime join_time, bool self) :
	m_self(self),
	m_name(name),
	m_address(address),
	m_join_time(join_time)
{
	for (auto i = 0; i < m_inputs.capacity(); i++)
		m_inputs.push_back(netplay_input());

	for (auto i = 0; i < m_predicted_inputs.capacity(); i++)
		m_predicted_inputs.push_back(netplay_input());
}

netplay_input& netplay_peer::get_next_input_buffer()
{
	m_inputs.advance(1);
	return m_inputs.newest();
}

netplay_input* netplay_peer::get_inputs_for(netplay_frame frame_index)
{
	for (auto& input : m_inputs)
	{
		if (input.m_frame_index == frame_index)
		{
			return &input;
		}
	}

	return nullptr;
}

netplay_input* netplay_peer::get_predicted_inputs_for(netplay_frame frame_index)
{
	for (auto& input : m_predicted_inputs)
	{
		if (input.m_frame_index == frame_index)
		{
			return &input;
		}
	}

	return nullptr;
}

double netplay_peer::average_latency()
{
	double avg_latency = 0.0;

	for (auto& latency : m_ping_history)
	{
		avg_latency += latency;
	}

	avg_latency /= (double)m_ping_history.size();
	return avg_latency;
}

double netplay_peer::highest_latency()
{
	double highest_latency = 0.0;

	for (auto& latency : m_ping_history)
	{
		highest_latency = std::max(latency, highest_latency);
	}

	return highest_latency;
}
