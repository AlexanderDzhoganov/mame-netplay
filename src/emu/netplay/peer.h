#ifndef MAME_EMU_NETPLAY_PEER_H
#define MAME_EMU_NETPLAY_PEER_H

typedef netplay_circular_buffer<netplay_input, 30> netplay_input_buffer;
typedef netplay_circular_buffer<double, 15> netplay_ping_history;

// this is the trivial imput predictor
// it simply repeats the previous frame inputs
class netplay_dummy_predictor
{
public:
	bool operator() (const netplay_input_buffer& inputs, netplay_input& predicted, netplay_frame frame_index)
	{
		if (inputs.empty())
			return false;

		predicted = inputs.newest();
		predicted.m_frame_index = frame_index;
		return true;
	}
};

class netplay_peer
{
	friend class netplay_manager;

	DISABLE_COPYING(netplay_peer);

public:
	netplay_peer(const std::string& name, const netplay_addr& address, attotime join_time, bool self = false);
	
	netplay_input& get_next_input_buffer();
	netplay_input* inputs_for(netplay_frame frame_index);
	netplay_input* predicted_inputs_for(netplay_frame frame_index);

	template <typename Predictor>
	netplay_input* predict_input_state(netplay_frame frame_index)
	{
		Predictor predictor;

		m_predicted_inputs.advance(1);
		auto& predicted = m_predicted_inputs.newest();
		if(predictor(m_inputs, predicted, frame_index))
		{
			return &predicted;
		}

		return nullptr;
	}

	double average_latency();
	double highest_latency();
	void add_latency_measurement(double latency) { m_ping_history.push_back(latency); }

	bool self() const { return m_self; }
	const std::string& name() const { return m_name; }
	attotime join_time() const { return m_join_time; }
	const netplay_addr& address() const { return m_address; }
	const netplay_input_buffer& inputs() const { return m_inputs; }
	const netplay_input_buffer& predicted_inputs() const { return m_predicted_inputs; }
	const netplay_ping_history& ping_history() const { return m_ping_history; }

private:
	bool m_self;                             // whether this is our peer
	std::string m_name;                      // the peer's self-specified name
	netplay_addr m_address;                  // the peer's network address
	attotime m_join_time;                    // when the peer joined
	netplay_input_buffer m_inputs;           // peer input buffer
	netplay_input_buffer m_predicted_inputs; // predicted inputs buffer
	netplay_ping_history m_ping_history;     // latency measurements history
	netplay_frame m_last_input_frame;        // last frame when we've seen inputs from this peer
};

#endif
