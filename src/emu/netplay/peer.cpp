#include <string>
#include <memory>

#include "attotime.h"
#include "netplay.h"
#include "netplay/input_state.h"
#include "netplay/peer.h"

netplay_latency_estimator::netplay_latency_estimator() :
	m_exp_alpha(0.05f),
	m_last_avg_value(50.0f)
{
	// add an initial sample of 50ms
	m_history.push_back(50.0f);
}

void netplay_latency_estimator::add_sample(float latency_ms)
{
	latency_ms = std::max(1.0f, std::min(250.0f, latency_ms));
	m_history.push_back(latency_ms);
}

float netplay_latency_estimator::predicted_latency()
{
	if (m_history.empty())
		return m_last_avg_value;

	float avg = m_last_avg_value;
	float high = 0.0f;
	float low = std::numeric_limits<float>::max();

	for (auto& sample : m_history)
	{
		if (sample > high)
			high = sample;
		if (sample < low)
			low = sample;

		// exponential moving average
		avg = sample * m_exp_alpha + avg * (1.0f - m_exp_alpha);
	}

	m_last_avg_value = avg;

	float confidence = 1.0f - std::min(1.0f, (high - low) / 50.0f);
	return avg * confidence + high * (1.0f - confidence);
}

netplay_peer::netplay_peer(const netplay_addr& address, attotime join_time, bool self) :
	m_state(NETPLAY_PEER_DISCONNECTED),
	m_self(self),
	m_name("peer"),
	m_address(address),
	m_join_time(join_time),
	m_last_system_time(0, 0)
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

netplay_input* netplay_peer::inputs_for(netplay_frame frame_index)
{
	for (auto i = 0; i < m_inputs.size(); i++)
	{
		auto& input = m_inputs[i];
		if (input.m_frame_index == frame_index)
			return &input;
	}

	return nullptr;
}

netplay_input* netplay_peer::predicted_inputs_for(netplay_frame frame_index)
{
	for (auto i = 0; i < m_predicted_inputs.size(); i++)
	{
		auto& input = m_predicted_inputs[i];
		if (input.m_frame_index == frame_index)
			return &input;
	}

	return nullptr;
}
