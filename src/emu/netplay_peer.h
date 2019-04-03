#ifndef MAME_EMU_NETPLAY_PEER_H
#define MAME_EMU_NETPLAY_PEER_H

class netplay_peer
{
	friend class netplay_manager;

	DISABLE_COPYING(netplay_peer);

	typedef netplay_circular_buffer<std::shared_ptr<netplay_input>, 60> netplay_input_buffer;

public:
	netplay_peer(const std::string& name, const netplay_addr& address, attotime join_time, bool self = false);
	void add_input_state(std::unique_ptr<netplay_input> input_state);

	netplay_input* get_inputs_for(unsigned long long frame_index);
	netplay_input* get_latest_input();
	int calculate_avg_latency() const;

	bool self() const { return m_self; }
	const std::string& name() const { return m_name; }
	attotime join_time() const { return m_join_time; }
	const netplay_addr& address() const { return m_address; }
	const netplay_input_buffer& inputs() const { return m_inputs; }

protected:
	bool m_self;                                        // whether this is our peer
	std::string m_name;                                 // the peer's self-specified name
	netplay_addr m_address;                             // the peer's network address
	attotime m_join_time;                               // when the peer joined
	netplay_input_buffer m_inputs;                      // peer input buffer
	netplay_circular_buffer<int, 50> m_latency_history; // latency measurements for the last 50 packets
};

#endif
