#include "base_node_connection.h"
#include "utility/logger.h"

#include <boost/program_options.hpp>

namespace po = boost::program_options;
using namespace beam;
using namespace ECC;


Initializer g_Initializer;

BaseTestNodeConnection::BaseTestNodeConnection(int argc, char* argv[])
	: m_Reactor(io::Reactor::create())
	, m_Scope(*m_Reactor)
	, m_Timer(io::Timer::create(io::Reactor::get_Current().shared_from_this()))
	, m_Failed(false)
	, m_Timeout(5 * 1000)
	
{
	ParseCommandLine(argc, argv);
	InitKdf();	
}

void BaseTestNodeConnection::Run()
{
	io::Address addr;
	addr.resolve(m_VM["address"].as<std::string>().c_str());
	addr.port(m_VM["port"].as<uint16_t>());

	Connect(addr);

	m_Reactor->run();
}

int BaseTestNodeConnection::CheckOnFailed()
{
	return m_Failed;
}

void BaseTestNodeConnection::ParseCommandLine(int argc, char* argv[])
{
	po::options_description options("allowed options");

	options.add_options()
		("address", po::value<std::string>()->default_value("127.0.0.1"), "ip address")
		("port", po::value<uint16_t>()->default_value(10000), "port")
		("wallet_seed", po::value<std::string>()->default_value("321"), "wallet seed");

	po::store(po::command_line_parser(argc, argv).options(options).run(), m_VM);
}

void BaseTestNodeConnection::InitKdf()
{
	NoLeak<uintBig> walletSeed;
	Hash::Value hv;

	Hash::Processor() << m_VM["wallet_seed"].as<std::string>().c_str() >> hv;
	walletSeed.V = hv;

	m_Kdf.m_Secret = walletSeed;
}

void BaseTestNodeConnection::OnConnected()
{
	LOG_INFO() << "connection is succeded";

	if (m_Timeout > 0)
	{
		m_Timer->start(m_Timeout, false, [this]()
		{
			LOG_INFO() << "Timeout";
			io::Reactor::get_Current().stop();
			m_Failed = true;
		});
	}

	GenerateTests();
	m_Index = 0;
	RunTest();
}

void BaseTestNodeConnection::OnClosed(int errorCode)
{
	LOG_ERROR() << "problem with connecting to node: code = " << io::error_str(static_cast<io::ErrorCode>(errorCode));
	m_Failed = true;
	io::Reactor::get_Current().stop();
}

void BaseTestNodeConnection::RunTest()
{
	if (m_Index < m_Tests.size())
		m_Tests[m_Index]();
}

void BaseTestNodeConnection::GenerateTests()
{	
}