
#include <boost/asio.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/bind.hpp>
#include <boost/thread/mutex.hpp>
#include <map>
#include <iostream>
#include <string>
#include <optional>
#include <queue>

using TcpSocket = boost::asio::ip::tcp::socket;
using DataBuffer = std::array<unsigned char, 8182>;
using TcpEndpoint = boost::asio::ip::tcp::endpoint;
using TcpAcceptor = boost::asio::ip::tcp::acceptor;
using IpV4 = boost::asio::ip::address_v4;
using Mutex = boost::mutex;
using Ptr = void*;
using ServerDataHandler = std::function<std::optional<int>(void*, std::string_view const&)>;
using ConnectHandler = std::function<void(void*)>;
using DisconnectHandler = std::function<void(void*)>;

class Bridge : public boost::enable_shared_from_this<Bridge>
{
	TcpSocket clientSocket;
	TcpSocket serverSocket;

	DataBuffer clientBuffer;
	DataBuffer serverBuffer;

	Mutex mutex;
	Mutex queueMutex;

	ServerDataHandler onServerData;
	DisconnectHandler onDisconnect;

public:

	Bridge(boost::asio::io_service& ios, ServerDataHandler const& onServerData, DisconnectHandler const& onDisconnect) :
		clientSocket(ios), serverSocket(ios),
		clientBuffer({}), serverBuffer({}),
		onServerData(onServerData),
		onDisconnect(onDisconnect)
	{

	}

	TcpSocket& ClientSocket()
	{
		return clientSocket;
	}

	TcpSocket& ServerSocket()
	{
		return serverSocket;
	}

	void ConnectToServer(std::string const& serverAddress, uint16_t serverPort)
	{
		serverSocket.async_connect(
			boost::asio::ip::tcp::endpoint(boost::asio::ip::address::from_string(serverAddress), serverPort),
			boost::bind(&Bridge::HandleServerConnect, shared_from_this(), boost::asio::placeholders::error)
		);
	}

	void HandleServerConnect(const boost::system::error_code& error)
	{
		if (!error)
		{
			serverSocket.async_read_some(
				boost::asio::buffer(serverBuffer.data(), serverBuffer.size()),
				boost::bind(&Bridge::HandleServerRead, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)
			);

			clientSocket.async_read_some(
				boost::asio::buffer(clientBuffer.data(), clientBuffer.size()),
				boost::bind(&Bridge::HandleClientRead, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)
			);
		}
		else
			Close();
	}

	void SendAsync(std::string const& msg)
	{
		serverSocket.async_write_some(
			boost::asio::buffer(msg.data(), msg.size()),
			boost::bind(&Bridge::HandleClientWrite, shared_from_this(), boost::asio::placeholders::error)
		);
	}

	void HandleServerRead(const boost::system::error_code& error, const size_t& bytesRead)
	{
		if (!error)
		{
			onServerData(this, std::string_view((char*)serverBuffer.data(), bytesRead));

			clientSocket.async_write_some(
				boost::asio::buffer(serverBuffer.data(), bytesRead),
				boost::bind(&Bridge::HandleClientWrite, shared_from_this(), boost::asio::placeholders::error)
			);
		}
		else
			Close();
	}

	void HandleClientRead(const boost::system::error_code& error, const size_t& bytesRead)
	{
		if (!error)
		{
			serverSocket.async_write_some(
				boost::asio::buffer(clientBuffer.data(), bytesRead),
				boost::bind(&Bridge::HandleServerWrite, shared_from_this(), boost::asio::placeholders::error)
			);
		}
		else
			Close();
	}

	void HandleServerWrite(const boost::system::error_code& error)
	{
		if (!error)
		{
			clientSocket.async_read_some(
				boost::asio::buffer(clientBuffer.data(), clientBuffer.size()),
				boost::bind(&Bridge::HandleClientRead, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)
			);
		}
		else
			Close();
	}

	void HandleClientWrite(const boost::system::error_code& error)
	{
		if (!error)
		{
			serverSocket.async_read_some(
				boost::asio::buffer(serverBuffer.data(), serverBuffer.size()),
				boost::bind(&Bridge::HandleServerRead, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)
			);
		}
		else
			Close();
	}

	void Close()
	{
		boost::mutex::scoped_lock lock(mutex);

		if (clientSocket.is_open())
		{
			clientSocket.close();
		}

		if (clientSocket.is_open())
		{
			clientSocket.close();
		}

		onDisconnect(this);
	}
};

class Proxy
{
	TcpAcceptor acceptor;
	boost::asio::io_service& ios;

	IpV4 localAddress;

	std::string serverAddress;
	uint16_t serverPort;

	std::map<void*, boost::weak_ptr<Bridge>> clients;

	Mutex mutex;

	ServerDataHandler onServerData;
	ConnectHandler onConnect;
	DisconnectHandler onDisconnect;

public:

	Proxy(
		boost::asio::io_service& ios,
		std::string const& localIp, uint16_t localPort, 
		std::string const& forwardIp, uint16_t forwardPort,
		ServerDataHandler&& serverDataHandler,
		ConnectHandler&& connectHandler,
		DisconnectHandler&& disconnectHandler) :
			ios(ios),
			localAddress(boost::asio::ip::address_v4::from_string(localIp)),
			acceptor(ios, TcpEndpoint(localAddress, localPort)),
			serverPort(forwardPort),
			serverAddress(forwardIp),
			onServerData(serverDataHandler),
			onConnect(connectHandler),
			onDisconnect(disconnectHandler)
	{

	}
		
	~Proxy()
	{
		Stop();
	}

	void Run()
	{
		AcceptAsync();

		CreateThread(0, 0, [](LPVOID pData) -> DWORD { return (DWORD)reinterpret_cast<Proxy*>(pData)->ios.run(); }, this, 0, 0);
	}

	void AcceptAsync()
	{
		auto connection = boost::shared_ptr<Bridge>(new Bridge(
			ios,
			onServerData,
			onDisconnect
		));

		acceptor.async_accept(
			connection->ClientSocket(),
			boost::bind(&Proxy::HandleAccept, this, connection, boost::asio::placeholders::error)
		);
	}

	void Stop()
	{
		boost::mutex::scoped_lock(mutex);

		for (auto& [id, cnx] : clients)
		{
			if (!cnx.expired())
				cnx.lock()->Close();
		}

		ios.stop();
	}

	void ClientSendAsync(void* ptr, std::string const& msg)
	{
		if (clients.contains(ptr))
		{
			auto client = clients[ptr].lock();

			client->ServerSocket().async_write_some(
				boost::asio::buffer(msg.data(), msg.size()),
				boost::bind(&Bridge::HandleServerWrite, client, boost::asio::placeholders::error)
			);
		}
	}

	std::optional<boost::shared_ptr<Bridge>> Client(void* ptr)
	{
		if (clients.contains(ptr))
			return clients[ptr].lock();

		return {};
	}

private:

	void RemoveClient(void* ptr)
	{
		boost::mutex::scoped_lock(mutex);

		if (clients.contains(ptr))
			clients.erase(ptr);
	}

	void HandleAccept(boost::shared_ptr<Bridge> connection, const boost::system::error_code& error)
	{
		void* ptr = connection.get();
		{
			boost::mutex::scoped_lock(mutex);
			clients[ptr] = connection;
		}

		connection->ConnectToServer(serverAddress, serverPort);

		AcceptAsync();
	}
};
