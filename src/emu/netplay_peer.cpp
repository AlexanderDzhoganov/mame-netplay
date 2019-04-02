#include "emu.h"
#include "netplay_input.h"
#include "netplay_peer.h"

netplay_peer::netplay_peer(const std::string& name, const netplay_address& address, attotime join_time, bool self) :
	m_self(self), m_name(name), m_address(address), m_join_time(join_time) {}

void netplay_peer::add_input_state(std::unique_ptr<netplay_input> input_state)
{
	m_inputs.push_back(std::move(input_state));
}

std::vector<netplay_input*> netplay_peer::get_inputs_before(attotime before_time)
{
	std::vector<netplay_input*> inputs;

	for (auto& input : m_inputs)
	{
		if (!input->consumed() && input->calculate_future_time(50) <= before_time) // NETPLAY TODO: pass proper latency
		{
			inputs.push_back(input.get());
		}
	}

	return inputs;
}

void netplay_peer::delete_inputs_before(attotime before_time)
{
	for (auto it = m_inputs.begin(); it != m_inputs.end(); ++it)
	{
		if ((*it)->m_timestamp <= before_time)
		{
			it = m_inputs.erase(it);
			if (it == m_inputs.end())
			{
				break;
			}
		}
	}
}
