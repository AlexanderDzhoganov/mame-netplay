#ifndef MAME_EMU_NETPLAY_PEER_H
#define MAME_EMU_NETPLAY_PEER_H

typedef std::unordered_map<netplay_frame, netplay_input> netplay_input_buffer;
typedef netplay_circular_buffer<float, 200> netplay_latency_samples;

// this is the trivial input predictor
// it simply repeats the previous frame inputs
class netplay_dummy_predictor
{
public:
	void operator() (const netplay_input_buffer& inputs, netplay_input& predicted, netplay_frame frame_index)
	{
		for (auto i = frame_index - 1; i > 0; i--)
		{
			auto it = inputs.find(i);
			if (it == inputs.end())
				continue;

			predicted = it->second;
			predicted.m_frame_index = frame_index;
			return;
		}
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
	friend class ioport_manager;

	DISABLE_COPYING(netplay_peer);

public:
	netplay_peer(unsigned char peerid, const netplay_addr& address, bool self = false);
	
	netplay_input* inputs_for(netplay_frame frame_index);
	netplay_input* predicted_inputs_for(netplay_frame frame_index);

	template <typename Predictor>
	netplay_input* predict_input_state(netplay_frame frame_index)
	{
		Predictor predictor;
		netplay_assert(m_predicted_inputs.find(frame_index) == m_predicted_inputs.end());
		auto& predicted = m_predicted_inputs[frame_index];
		predicted.m_frame_index = frame_index;
		predictor(m_inputs, predicted, frame_index);
		return &predicted;
	}

	bool self() const { return m_self; }
	unsigned char peerid() const { return m_peerid; }
	const std::string& name() const { return m_name; }
	const netplay_addr& address() const { return m_address; }
	const netplay_input_buffer& inputs() const { return m_inputs; }
	const netplay_input_buffer& predicted_inputs() const { return m_predicted_inputs; }
	bool dirty() const { return !m_predicted_inputs.empty(); }
	void gc_buffers(netplay_frame before_frame);

	static void gc_buffer(netplay_frame before_frame, netplay_input_buffer& buffer, bool warn);
 
	netplay_latency_estimator& latency_estimator() { return m_latency_estimator; }

private:
	unsigned char m_peerid;
	netplay_peer_state m_state;
	bool m_self;                             // whether this is our peer
	std::string m_name;                      // the peer's self-specified name
	netplay_addr m_address;                  // the peer's network address
	netplay_input_buffer m_inputs;           // peer input buffer
	netplay_input_buffer m_predicted_inputs; // predicted inputs buffer
	attotime m_last_system_time;             // the last system time we've received from this peer
	netplay_latency_estimator m_latency_estimator;
	std::unordered_map<netplay_frame, std::vector<unsigned int>> m_checksums;
	netplay_frame m_next_inputs_at;
};

#endif
