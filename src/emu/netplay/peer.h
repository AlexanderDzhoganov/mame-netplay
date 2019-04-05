#ifndef MAME_EMU_NETPLAY_PEER_H
#define MAME_EMU_NETPLAY_PEER_H

typedef netplay_circular_buffer<std::shared_ptr<netplay_input>, 30> netplay_input_buffer;
typedef netplay_circular_buffer<double, 10> netplay_ping_history;

// this is the trivial imput predictor
// it simply repeats the previous frame inputs
class netplay_dummy_predictor
{
public:
	std::unique_ptr<netplay_input> operator() (const netplay_input_buffer& inputs, unsigned long long frame_index)
	{
		return inputs.empty() ? nullptr : std::make_unique<netplay_input>(*inputs.newest());
	}
};

class netplay_peer
{
	friend class netplay_manager;

	DISABLE_COPYING(netplay_peer);

public:
	netplay_peer(const std::string& name, const netplay_addr& address, attotime join_time, bool self = false);
	void add_input_state(std::unique_ptr<netplay_input> input_state);

	template <typename Predictor>
	netplay_input* predict_input_state(unsigned long long frame_index)
	{
		Predictor predictor;
		auto predicted = predictor(m_inputs, frame_index);

		if (predicted == nullptr)
		{
			return nullptr;
		}

		auto predicted_ptr = predicted.get();
		m_predicted_inputs.push_back(std::move(predicted));
		return predicted_ptr;
	}

	netplay_input* get_inputs_for(unsigned long long frame_index);
	netplay_input* get_predicted_inputs_for(unsigned long long frame_index);
	double average_latency();
	void add_latency_measurement(double latency) { m_ping_history.push_back(latency); }

	bool self() const { return m_self; }
	const std::string& name() const { return m_name; }
	attotime join_time() const { return m_join_time; }
	const netplay_addr& address() const { return m_address; }
	const netplay_input_buffer& inputs() const { return m_inputs; }
	const netplay_input_buffer& predicted_inputs() const { return m_predicted_inputs; }
	const netplay_ping_history& ping_history() const { return m_ping_history; }

protected:
	bool m_self;                             // whether this is our peer
	std::string m_name;                      // the peer's self-specified name
	netplay_addr m_address;                  // the peer's network address
	attotime m_join_time;                    // when the peer joined
	netplay_input_buffer m_inputs;           // peer input buffer
	netplay_input_buffer m_predicted_inputs; // predicted inputs buffer
	netplay_ping_history m_ping_history;
};

#endif
