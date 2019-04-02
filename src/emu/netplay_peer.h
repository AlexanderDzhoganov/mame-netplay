#ifndef MAME_EMU_NETPLAY_PEER_H
#define MAME_EMU_NETPLAY_PEER_H

class netplay_peer
{
	DISABLE_COPYING(netplay_peer);

public:
	netplay_peer(const std::string& name, const netplay_address& address, attotime join_time, bool self = false);
	void add_input_state(std::unique_ptr<netplay_input> input_state);

	std::vector<netplay_input*> get_inputs_before(attotime before_time);
	void delete_inputs_before(attotime before_time);

	bool self() const { return m_self; }
	const std::string& name() const { return m_name; }
	void set_name(const std::string& name) { m_name = name; }
	attotime join_time() const { return m_join_time; }
	void set_join_time(const attotime& join_time) { m_join_time = join_time; }
	const netplay_address& address() const { return m_address; }
	const std::list<std::shared_ptr<netplay_input>>& inputs() const { return m_inputs; }

private:
	bool m_self; // peer is this node
	std::string m_name;
	netplay_address m_address;
	attotime m_join_time;
	std::list<std::shared_ptr<netplay_input>> m_inputs;
};

#endif
