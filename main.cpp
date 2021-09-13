int main()
{
	boost::asio::io_service ios;

	Proxy proxy(
		ios,
		"127.0.0.1", 5000,
		"127.0.0.1", 5001,
		[](auto* p, auto& data) -> std::optional<int> { std::cout << "Received " << data.data() << " from 0x" << std::hex << p << std::endl;  return {}; },
		[](auto* p) -> void { std::cout << "Bridge 0x" << std::hex << p << " established" << std::endl; },
		[](auto* p) -> void { std::cout << "Bridge 0x" << std::hex << p << " shutdown" << std::endl; }
	);

	proxy.AcceptAsync();

	return ios.run();
}
