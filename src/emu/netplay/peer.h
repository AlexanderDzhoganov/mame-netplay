#ifndef MAME_EMU_NETPLAY_PEER_H
#define MAME_EMU_NETPLAY_PEER_H

typedef netplay_circular_buffer<netplay_input, 30> netplay_input_buffer;
typedef netplay_circular_buffer<float, 180> netplay_latency_samples;

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

class netplay_latency_estimator
{
	public:
	netplay_latency_estimator();

	void add_sample(float latency_ms);
	float predicted_latency();

	private:
	netplay_latency_samples m_history;
	float m_exp_alpha;
	float m_last_avg_value;
};

enum netplay_peer_state
{
	NETPLAY_PEER_DISCONNECTED = 0,
	NETPLAY_PEER_NOT_READY,
	NETPLAY_PEER_SYNCING,
	NETPLAY_PEER_ONLINE
};

class netplay_peer
{
	friend class netplay_manager;

	DISABLE_COPYING(netplay_peer);

public:
	netplay_peer(const netplay_addr& address, attotime join_time, bool self = false);
	
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

	netplay_peer_state state() const { return m_state; }
	void set_state(netplay_peer_state state) { m_state = state; }

	bool self() const { return m_self; }
	const std::string& name() const { return m_name; }
	const attotime& join_time() const { return m_join_time; }
	const netplay_addr& address() const { return m_address; }
	const netplay_input_buffer& inputs() const { return m_inputs; }
	const netplay_input_buffer& predicted_inputs() const { return m_predicted_inputs; }

	netplay_latency_estimator& latency_estimator() { return m_latency_estimator; }

private:
	netplay_peer_state m_state;
	bool m_self;                             // whether this is our peer
	std::string m_name;                      // the peer's self-specified name
	netplay_addr m_address;                  // the peer's network address
	attotime m_join_time;                    // when the peer joined
	netplay_input_buffer m_inputs;           // peer input buffer
	netplay_input_buffer m_predicted_inputs; // predicted inputs buffer
	netplay_frame m_last_input_frame;        // last frame when we've seen inputs from this peer
	attotime m_last_system_time;             // the last system time we've received from this peer
	netplay_latency_estimator m_latency_estimator;
};

#endif
