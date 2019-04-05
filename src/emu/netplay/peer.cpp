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
	m_join_time(join_time) {}

void netplay_peer::add_input_state(std::unique_ptr<netplay_input> input_state)
{
	m_inputs.push_back(std::move(input_state));
}

netplay_input* netplay_peer::get_inputs_for(netplay_frame frame_index)
{
	for (auto& input : m_inputs)
	{
		if (input->m_frame_index == frame_index)
		{
			return input.get();
		}
	}

	return nullptr;
}

netplay_input* netplay_peer::get_predicted_inputs_for(netplay_frame frame_index)
{
	for (auto& input : m_predicted_inputs)
	{
		if (input->m_frame_index == frame_index)
		{
			return input.get();
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
