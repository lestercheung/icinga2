/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "icingadb/redisconnection.hpp"
#include "base/array.hpp"
#include "base/convert.hpp"
#include "base/defer.hpp"
#include "base/exception.hpp"
#include "base/io-engine.hpp"
#include "base/logger.hpp"
#include "base/objectlock.hpp"
#include "base/string.hpp"
#include "base/tcpsocket.hpp"
#include "base/tlsutility.hpp"
#include <boost/asio.hpp>
#include <boost/coroutine/exceptions.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/variant/get.hpp>
#include <exception>
#include <future>
#include <iterator>
#include <memory>
#include <utility>

using namespace icinga;
namespace asio = boost::asio;

RedisConnection::RedisConnection(const String& host, int port, const String& path, const String& password, int db,
	bool useTls, const String& certPath, const String& keyPath, const String& caPath, const String& crlPath,
	const String& tlsProtocolmin, const String& cipherList, double tlsHandshakeTimeout)
	: RedisConnection(IoEngine::Get().GetIoContext(), host, port, path, password, db,
	  useTls, certPath, keyPath, caPath, crlPath, tlsProtocolmin, cipherList, tlsHandshakeTimeout)
{
}

RedisConnection::RedisConnection(boost::asio::io_context& io, String host, int port, String path, String password,
	int db, bool useTls, String certPath, String keyPath, String caPath, String crlPath,
	String tlsProtocolmin, String cipherList, double tlsHandshakeTimeout)
	: m_Host(std::move(host)), m_Port(port), m_Path(std::move(path)), m_Password(std::move(password)),
	  m_DbIndex(db), m_CertPath(std::move(certPath)), m_KeyPath(std::move(keyPath)),
	  m_CaPath(std::move(caPath)), m_CrlPath(std::move(crlPath)), m_TlsProtocolmin(std::move(tlsProtocolmin)),
	  m_CipherList(std::move(cipherList)), m_TlsHandshakeTimeout(tlsHandshakeTimeout),
	  m_Connecting(false), m_Connected(false), m_Started(false), m_Strand(io), m_QueuedWrites(io), m_QueuedReads(io)
{
	if (useTls) {
		UpdateSSLContext();
	}
}

void RedisConnection::UpdateSSLContext()
{
	namespace ssl = boost::asio::ssl;

	Shared<ssl::context>::Ptr context;

	try {
		context = MakeAsioSslContext(m_CertPath, m_KeyPath, m_CaPath);
	} catch (const std::exception& ex) {
		BOOST_THROW_EXCEPTION(ScriptError("Cannot make SSL context: " + DiagnosticInformation(ex) + "; cert path: '"
			+ m_CertPath + "'; key path: '" + m_KeyPath + "'; ca path: '" + m_CaPath + "'."));
	}

	if (!m_CrlPath.IsEmpty()) {
		try {
			AddCRLToSSLContext(context, m_CrlPath);
		} catch (const std::exception& ex) {
			BOOST_THROW_EXCEPTION(ScriptError("Cannot add certificate revocation list to SSL context: "
				+ DiagnosticInformation(ex) + "; crl path: '" + m_CrlPath + "'."));
		}
	}

	if (!m_CipherList.IsEmpty()) {
		try {
			SetCipherListToSSLContext(context, m_CipherList);
		} catch (const std::exception& ex) {
			BOOST_THROW_EXCEPTION(ScriptError("Cannot set cipher list to SSL context: "
				+ DiagnosticInformation(ex) + "; cipher list: '" + m_CipherList + "'."));
		}
	}

	if (!m_TlsProtocolmin.IsEmpty()){
		try {
			SetTlsProtocolminToSSLContext(context, m_TlsProtocolmin);
		} catch (const std::exception& ex) {
			BOOST_THROW_EXCEPTION(ScriptError("Cannot set minimum TLS protocol version to SSL context: "
				+ DiagnosticInformation(ex) + "; tls_protocolmin: '" + GetTlsProtocolmin() + "'."));
		}
	}

	m_SSLContext = std::move(context);
}

void RedisConnection::Start()
{
	if (!m_Started.exchange(true)) {
		Ptr keepAlive (this);

		IoEngine::SpawnCoroutine(m_Strand, [this, keepAlive](asio::yield_context yc) { ReadLoop(yc); });
		IoEngine::SpawnCoroutine(m_Strand, [this, keepAlive](asio::yield_context yc) { WriteLoop(yc); });
	}

	if (!m_Connecting.exchange(true)) {
		Ptr keepAlive (this);

		IoEngine::SpawnCoroutine(m_Strand, [this, keepAlive](asio::yield_context yc) { Connect(yc); });
	}
}

bool RedisConnection::IsConnected() {
	return m_Connected.load();
}

/**
 * Append a Redis query to a log message
 *
 * @param query Redis query
 * @param msg Log message
 */
static inline
void LogQuery(RedisConnection::Query& query, Log& msg)
{
	int i = 0;

	for (auto& arg : query) {
		if (++i == 8) {
			msg << " ...";
			break;
		}

		if (arg.GetLength() > 64) {
			msg << " '" << arg.SubStr(0, 61) << "...'";
		} else {
			msg << " '" << arg << '\'';
		}
	}
}

/**
 * Queue a Redis query for sending
 *
 * @param query Redis query
 * @param priority The query's priority
 */
void RedisConnection::FireAndForgetQuery(RedisConnection::Query query, RedisConnection::QueryPriority priority)
{
	{
		Log msg (LogNotice, "IcingaDB", "Firing and forgetting query:");
		LogQuery(query, msg);
	}

	auto item (Shared<Query>::Make(std::move(query)));

	asio::post(m_Strand, [this, item, priority]() {
		m_Queues.Writes[priority].emplace(WriteQueueItem{item, nullptr, nullptr, nullptr});
		m_QueuedWrites.Set();
	});
}

/**
 * Queue Redis queries for sending
 *
 * @param queries Redis queries
 * @param priority The queries' priority
 */
void RedisConnection::FireAndForgetQueries(RedisConnection::Queries queries, RedisConnection::QueryPriority priority)
{
	for (auto& query : queries) {
		Log msg (LogNotice, "IcingaDB", "Firing and forgetting query:");
		LogQuery(query, msg);
	}

	auto item (Shared<Queries>::Make(std::move(queries)));

	asio::post(m_Strand, [this, item, priority]() {
		m_Queues.Writes[priority].emplace(WriteQueueItem{nullptr, item, nullptr, nullptr});
		m_QueuedWrites.Set();
	});
}

/**
 * Queue a Redis query for sending, wait for the response and return (or throw) it
 *
 * @param query Redis query
 * @param priority The query's priority
 *
 * @return The response
 */
RedisConnection::Reply RedisConnection::GetResultOfQuery(RedisConnection::Query query, RedisConnection::QueryPriority priority)
{
	{
		Log msg (LogNotice, "IcingaDB", "Executing query:");
		LogQuery(query, msg);
	}

	std::promise<Reply> promise;
	auto future (promise.get_future());
	auto item (Shared<std::pair<Query, std::promise<Reply>>>::Make(std::move(query), std::move(promise)));

	asio::post(m_Strand, [this, item, priority]() {
		m_Queues.Writes[priority].emplace(WriteQueueItem{nullptr, nullptr, item, nullptr});
		m_QueuedWrites.Set();
	});

	item = nullptr;
	future.wait();
	return future.get();
}

/**
 * Queue Redis queries for sending, wait for the responses and return (or throw) them
 *
 * @param queries Redis queries
 * @param priority The queries' priority
 *
 * @return The responses
 */
RedisConnection::Replies RedisConnection::GetResultsOfQueries(RedisConnection::Queries queries, RedisConnection::QueryPriority priority)
{
	for (auto& query : queries) {
		Log msg (LogNotice, "IcingaDB", "Executing query:");
		LogQuery(query, msg);
	}

	std::promise<Replies> promise;
	auto future (promise.get_future());
	auto item (Shared<std::pair<Queries, std::promise<Replies>>>::Make(std::move(queries), std::move(promise)));

	asio::post(m_Strand, [this, item, priority]() {
		m_Queues.Writes[priority].emplace(WriteQueueItem{nullptr, nullptr, nullptr, item});
		m_QueuedWrites.Set();
	});

	item = nullptr;
	future.wait();
	return future.get();
}

/**
 * Mark kind as kind of queries not to actually send yet
 *
 * @param kind Query kind
 */
void RedisConnection::SuppressQueryKind(RedisConnection::QueryPriority kind)
{
	asio::post(m_Strand, [this, kind]() { m_SuppressedQueryKinds.emplace(kind); });
}

/**
 * Unmark kind as kind of queries not to actually send yet
 *
 * @param kind Query kind
 */
void RedisConnection::UnsuppressQueryKind(RedisConnection::QueryPriority kind)
{
	asio::post(m_Strand, [this, kind]() {
		m_SuppressedQueryKinds.erase(kind);
		m_QueuedWrites.Set();
	});
}

/**
 * Try to connect to Redis
 */
void RedisConnection::Connect(asio::yield_context& yc)
{
	Defer notConnecting ([this]() { m_Connecting.store(m_Connected.load()); });

	boost::asio::deadline_timer timer (m_Strand.context());

	for (;;) {
		try {
			if (m_Path.IsEmpty()) {
				Log(LogInformation, "IcingaDB")
					<< "Trying to connect to Redis server (async) on host '" << m_Host << ":" << m_Port << "'";

				auto conn (Shared<TcpConn>::Make(m_Strand.context()));
				icinga::Connect(conn->next_layer(), m_Host, Convert::ToString(m_Port), yc);
				m_TcpConn = std::move(conn);
			} else {
				Log(LogInformation, "IcingaDB")
					<< "Trying to connect to Redis server (async) on unix socket path '" << m_Path << "'";

				auto conn (Shared<UnixConn>::Make(m_Strand.context()));
				conn->next_layer().async_connect(Unix::endpoint(m_Path.CStr()), yc);
				m_UnixConn = std::move(conn);
			}

			m_Connected.store(true);

			Log(LogInformation, "IcingaDB", "Connected to Redis server");

			break;
		} catch (const boost::coroutines::detail::forced_unwind&) {
			throw;
		} catch (const std::exception& ex) {
			Log(LogCritical, "IcingaDB")
				<< "Cannot connect to " << m_Host << ":" << m_Port << ": " << ex.what();
		}

		timer.expires_from_now(boost::posix_time::seconds(5));
		timer.async_wait(yc);
	}

}

/**
 * Actually receive the responses to the Redis queries send by WriteItem() and handle them
 */
void RedisConnection::ReadLoop(asio::yield_context& yc)
{
	for (;;) {
		m_QueuedReads.Wait(yc);

		while (!m_Queues.FutureResponseActions.empty()) {
			auto item (std::move(m_Queues.FutureResponseActions.front()));
			m_Queues.FutureResponseActions.pop();

			switch (item.Action) {
				case ResponseAction::Ignore:
					try {
						for (auto i (item.Amount); i; --i) {
							ReadOne(yc);
						}
					} catch (const boost::coroutines::detail::forced_unwind&) {
						throw;
					} catch (const std::exception& ex) {
						Log(LogCritical, "IcingaDB")
							<< "Error during receiving the response to a query which has been fired and forgotten: " << ex.what();

						continue;
					} catch (...) {
						Log(LogCritical, "IcingaDB")
							<< "Error during receiving the response to a query which has been fired and forgotten";

						continue;
					}

					break;
				case ResponseAction::Deliver:
					for (auto i (item.Amount); i; --i) {
						auto promise (std::move(m_Queues.ReplyPromises.front()));
						m_Queues.ReplyPromises.pop();

						Reply reply;

						try {
							reply = ReadOne(yc);
						} catch (const boost::coroutines::detail::forced_unwind&) {
							throw;
						} catch (...) {
							promise.set_exception(std::current_exception());

							continue;
						}

						promise.set_value(std::move(reply));
					}

					break;
				case ResponseAction::DeliverBulk:
					{
						auto promise (std::move(m_Queues.RepliesPromises.front()));
						m_Queues.RepliesPromises.pop();

						Replies replies;
						replies.reserve(item.Amount);

						for (auto i (item.Amount); i; --i) {
							try {
								replies.emplace_back(ReadOne(yc));
							} catch (const boost::coroutines::detail::forced_unwind&) {
								throw;
							} catch (...) {
								promise.set_exception(std::current_exception());

								continue;
							}
						}

						promise.set_value(std::move(replies));
					}
			}
		}

		m_QueuedReads.Clear();
	}
}

/**
 * Actually send the Redis queries queued by {FireAndForget,GetResultsOf}{Query,Queries}()
 */
void RedisConnection::WriteLoop(asio::yield_context& yc)
{
	for (;;) {
		m_QueuedWrites.Wait(yc);

	WriteFirstOfHighestPrio:
		for (auto& queue : m_Queues.Writes) {
			if (m_SuppressedQueryKinds.find(queue.first) != m_SuppressedQueryKinds.end() || queue.second.empty()) {
				continue;
			}

			auto next (std::move(queue.second.front()));
			queue.second.pop();

			WriteItem(yc, std::move(next));

			goto WriteFirstOfHighestPrio;
		}

		m_QueuedWrites.Clear();
	}
}

/**
 * Send next and schedule receiving the response
 *
 * @param next Redis queries
 */
void RedisConnection::WriteItem(boost::asio::yield_context& yc, RedisConnection::WriteQueueItem next)
{
	if (next.FireAndForgetQuery) {
		auto& item (*next.FireAndForgetQuery);

		try {
			WriteOne(item, yc);
		} catch (const boost::coroutines::detail::forced_unwind&) {
			throw;
		} catch (const std::exception& ex) {
			Log msg (LogCritical, "IcingaDB", "Error during sending query");
			LogQuery(item, msg);
			msg << " which has been fired and forgotten: " << ex.what();

			return;
		} catch (...) {
			Log msg (LogCritical, "IcingaDB", "Error during sending query");
			LogQuery(item, msg);
			msg << " which has been fired and forgotten";

			return;
		}

		if (m_Queues.FutureResponseActions.empty() || m_Queues.FutureResponseActions.back().Action != ResponseAction::Ignore) {
			m_Queues.FutureResponseActions.emplace(FutureResponseAction{1, ResponseAction::Ignore});
		} else {
			++m_Queues.FutureResponseActions.back().Amount;
		}

		m_QueuedReads.Set();
	}

	if (next.FireAndForgetQueries) {
		auto& item (*next.FireAndForgetQueries);
		size_t i = 0;

		try {
			for (auto& query : item) {
				WriteOne(query, yc);
				++i;
			}
		} catch (const boost::coroutines::detail::forced_unwind&) {
			throw;
		} catch (const std::exception& ex) {
			Log msg (LogCritical, "IcingaDB", "Error during sending query");
			LogQuery(item[i], msg);
			msg << " which has been fired and forgotten: " << ex.what();

			return;
		} catch (...) {
			Log msg (LogCritical, "IcingaDB", "Error during sending query");
			LogQuery(item[i], msg);
			msg << " which has been fired and forgotten";

			return;
		}

		if (m_Queues.FutureResponseActions.empty() || m_Queues.FutureResponseActions.back().Action != ResponseAction::Ignore) {
			m_Queues.FutureResponseActions.emplace(FutureResponseAction{item.size(), ResponseAction::Ignore});
		} else {
			m_Queues.FutureResponseActions.back().Amount += item.size();
		}

		m_QueuedReads.Set();
	}

	if (next.GetResultOfQuery) {
		auto& item (*next.GetResultOfQuery);

		try {
			WriteOne(item.first, yc);
		} catch (const boost::coroutines::detail::forced_unwind&) {
			throw;
		} catch (...) {
			item.second.set_exception(std::current_exception());

			return;
		}

		m_Queues.ReplyPromises.emplace(std::move(item.second));

		if (m_Queues.FutureResponseActions.empty() || m_Queues.FutureResponseActions.back().Action != ResponseAction::Deliver) {
			m_Queues.FutureResponseActions.emplace(FutureResponseAction{1, ResponseAction::Deliver});
		} else {
			++m_Queues.FutureResponseActions.back().Amount;
		}

		m_QueuedReads.Set();
	}

	if (next.GetResultsOfQueries) {
		auto& item (*next.GetResultsOfQueries);

		try {
			for (auto& query : item.first) {
				WriteOne(query, yc);
			}
		} catch (const boost::coroutines::detail::forced_unwind&) {
			throw;
		} catch (...) {
			item.second.set_exception(std::current_exception());

			return;
		}

		m_Queues.RepliesPromises.emplace(std::move(item.second));
		m_Queues.FutureResponseActions.emplace(FutureResponseAction{item.first.size(), ResponseAction::DeliverBulk});

		m_QueuedReads.Set();
	}
}

/**
 * Receive the response to a Redis query
 *
 * @return The response
 */
RedisConnection::Reply RedisConnection::ReadOne(boost::asio::yield_context& yc)
{
	if (m_Path.IsEmpty()) {
		return ReadOne(m_TcpConn, yc);
	} else {
		return ReadOne(m_UnixConn, yc);
	}
}

/**
 * Send query
 *
 * @param query Redis query
 */
void RedisConnection::WriteOne(RedisConnection::Query& query, asio::yield_context& yc)
{
	if (m_Path.IsEmpty()) {
		WriteOne(m_TcpConn, query, yc);
	} else {
		WriteOne(m_UnixConn, query, yc);
	}
}
