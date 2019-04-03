#include "emu.h"
#include "netplay_util.h"
#include "netplay_input.h"
#include "netplay_peer.h"

netplay_peer::netplay_peer(const std::string& name, const netplay_addr& address, attotime join_time, bool self) :
	m_self(self),
	m_name(name),
	m_address(address),
	m_join_time(join_time) {}

void netplay_peer::add_input_state(std::unique_ptr<netplay_input> input_state)
{
	m_inputs.push_back(std::move(input_state));
}

netplay_input* netplay_peer::get_inputs_for(unsigned long long frame_index)
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

netplay_input* netplay_peer::get_latest_input()
{
	if (m_inputs.empty())
	{
		return nullptr;
	}

	return m_inputs.newest().get();
}

int netplay_peer::calculate_avg_latency() const
{
	int avg_latency_ms = 0;

	for (auto measurement : m_latency_history.items())
	{
		avg_latency_ms += measurement;
	}

	avg_latency_ms /= m_latency_history.size();

	return avg_latency_ms;
}
