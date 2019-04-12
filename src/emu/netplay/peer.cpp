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

	float confidence = 1.0f - std::min(1.0f, (high - low) / 250.0f);
	float predicted = avg * confidence + high * (1.0f - confidence);

	// static int every = 0;
	// if (every++ % 10 == 0)
	//	NETPLAY_LOG("low: %.2f, avg: %.2f, high: %.2f, confidence: %.2f, predicted: %.2f", low, avg, high, confidence, predicted);
	return predicted;
}

netplay_peer::netplay_peer(unsigned char peerid, const netplay_addr& address, attotime join_time, bool self) :
	m_peerid(peerid),
	m_state(NETPLAY_PEER_DISCONNECTED),
	m_self(self),
	m_name("peer"),
	m_address(address),
	m_join_time(join_time),
	m_last_system_time(0, 0),
	m_next_inputs_at(0) {}

netplay_input* netplay_peer::inputs_for(netplay_frame frame_index)
{
	auto it = m_inputs.find(frame_index);
	if (it == m_inputs.end())
		return nullptr;

	return &(it->second);
}

netplay_input* netplay_peer::predicted_inputs_for(netplay_frame frame_index)
{
	auto it = m_predicted_inputs.find(frame_index);
	if (it == m_predicted_inputs.end())
		return nullptr;

	return &(it->second);
}

void netplay_peer::gc_buffers(netplay_frame before_frame)
{
	gc_buffer(before_frame, m_inputs);
	gc_buffer(before_frame, m_predicted_inputs);
}

void netplay_peer::gc_buffer(netplay_frame before_frame, netplay_input_buffer& buffer)
{
	for (auto it = buffer.begin(); it != buffer.end(); ++it)
	{
		if (it->first >= before_frame)
			continue;

		it = buffer.erase(it);
		if (it == buffer.end())
			break;
	}
}
