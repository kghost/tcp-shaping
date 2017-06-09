#include "config.h"

#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <memory>
#include <iostream>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#if defined(HAVE_BOOST_LOG)
#define BOOST_LOG_USE_NATIVE_SYSLOG
#include <boost/make_shared.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sinks/syslog_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#else
#include "logger.hpp"
#endif

#include "error-code.hpp"

namespace po = boost::program_options;

class forwarder : public std::enable_shared_from_this<forwarder> {
	public:
		forwarder(boost::asio::io_service& io_service, std::shared_ptr<boost::asio::ip::tcp::socket> local)
			: io_service(io_service), local(local) {
				std::stringstream ss;
				ss << "forwarder(" << local->remote_endpoint() << " => " << local->local_endpoint() << ") ";
				tag = ss.str();
			}

		void start() {
			boost::asio::ip::tcp::no_delay option(true);
			local->non_blocking(true);
			local->set_option(option);
			auto endpoint = local->local_endpoint();
			remote->open(endpoint.protocol());
			if (endpoint.protocol().family() == AF_INET6)
				remote->set_option(boost::asio::ip::v6_only(true));
			remote->non_blocking(true);
			remote->set_option(option);
			BOOST_LOG_TRIVIAL(info) << tag << "connecting";
			remote->async_connect(endpoint, [me = shared_from_this(), this](const boost::system::error_code& ec) {
				if (!ec) {
					BOOST_LOG_TRIVIAL(info) << tag << "connected";
					int p[2][2];
					auto rc = ::pipe2(p[0], O_NONBLOCK);
					if (rc < 0) {
						char e[200];
						BOOST_LOG_TRIVIAL(error) << tag << " pipe: " << strerror_r(errno, e, sizeof(e));
						return;
					}
					rc = ::pipe2(p[1], O_NONBLOCK);
					if (rc < 0) {
						char e[200];
						BOOST_LOG_TRIVIAL(error) << tag << " pipe: " << strerror_r(errno, e, sizeof(e));
						return;
					}
					auto pipe11 = std::make_shared<boost::asio::posix::stream_descriptor>(io_service, p[0][0]);
					auto pipe12 = std::make_shared<boost::asio::posix::stream_descriptor>(io_service, p[0][1]);
					auto pipe21 = std::make_shared<boost::asio::posix::stream_descriptor>(io_service, p[1][0]);
					auto pipe22 = std::make_shared<boost::asio::posix::stream_descriptor>(io_service, p[1][1]);
					loop("l->p", local, pipe12);
					loop("p->r", pipe11, remote);
					loop("r->p", remote, pipe22);
					loop("p->l", pipe21, local);
				} else {
					BOOST_LOG_TRIVIAL(info) << tag << " connection failed";
				}
			});
		}

	private:
		template<typename INPUT, typename OUTPUT>
		void loop(const char *hint, INPUT input, OUTPUT output) {
			read(*input, [me = shared_from_this(), this, hint, input, output](const gh::error_code& ec, std::size_t bytes_transferred) {
				if (!ec) {
					auto eof = false;
					auto pause = false;
					for (auto av = std::max(available(*input), 1u); av > 0; av = available(*input)) {
						auto rc = ::splice(input->native_handle(), nullptr, output->native_handle(), nullptr, av, SPLICE_F_MOVE|SPLICE_F_NONBLOCK);
						if (rc == 0) {
							eof = true;
							BOOST_LOG_TRIVIAL(info) << tag << hint << " eof";
							shut(*output);
							break;
						} else if (rc < 0) {
							if (errno == EAGAIN) {
								pause = true;
								write(*output, [me, this, hint, input, output](const gh::error_code& ec, std::size_t bytes_transferred) {
									if (!ec) {
										loop(hint, input, output);
									} else {
										BOOST_LOG_TRIVIAL(error) << tag << hint << " write error: " << ec.message();
									}
								});
								break;
							} else {
								char e[200];
								BOOST_LOG_TRIVIAL(error) << tag << hint << " splice error: " << strerror_r(errno, e, sizeof(e));
								return;
							}
						}
					}
					if (!eof && !pause) loop(hint, input, output);
				} else {
					BOOST_LOG_TRIVIAL(error) << tag << hint << " read error: " << ec.message();
				}
			});
		}

		template<typename ReadHandler>
		static auto read(boost::asio::ip::tcp::socket& socket, ReadHandler h) {
			socket.async_receive(boost::asio::null_buffers(), h);
		}

		template<typename ReadHandler>
		static auto read(boost::asio::posix::stream_descriptor& pipe, ReadHandler h) {
			pipe.async_read_some(boost::asio::null_buffers(), h);
		}

		template<typename ReadHandler>
		static auto write(boost::asio::ip::tcp::socket& socket, ReadHandler h) {
			socket.async_send(boost::asio::null_buffers(), h);
		}

		template<typename ReadHandler>
		static auto write(boost::asio::posix::stream_descriptor& pipe, ReadHandler h) {
			pipe.async_write_some(boost::asio::null_buffers(), h);
		}

		static auto available(boost::asio::ip::tcp::socket& socket) {
			return socket.available();
		}

		static auto available(boost::asio::posix::stream_descriptor& pipe) {
			boost::asio::socket_base::bytes_readable command(true);
			pipe.io_control(command);
			return command.get();
		}

		static auto shut(boost::asio::ip::tcp::socket& socket) {
			boost::system::error_code ec;
			int keepalive_enabled = 1;
			int keepalive_time = 30;
			int keepalive_count = 3;
			int keepalive_interval = 10;
			setsockopt(socket.native_handle(), SOL_SOCKET, SO_KEEPALIVE, &keepalive_enabled, sizeof(keepalive_enabled));
			setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_time, sizeof(keepalive_time));
			setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_KEEPCNT, &keepalive_count, sizeof(keepalive_count));
			setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_interval, sizeof(keepalive_interval));
			return socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
		}

		static auto shut(boost::asio::posix::stream_descriptor& pipe) {
			return pipe.close();
		}

		boost::asio::io_service& io_service;
		std::shared_ptr<boost::asio::ip::tcp::socket> local;
		std::shared_ptr<boost::asio::ip::tcp::socket> remote = std::make_shared<boost::asio::ip::tcp::socket>(io_service);
		std::string tag;
};

class listener : public std::enable_shared_from_this<listener> {
	public:
		listener(boost::asio::io_service &io_service)
			: io_service(io_service), socket(io_service) {}

		void start(boost::asio::ip::tcp::endpoint endpoint) {
			try {
				socket.open(endpoint.protocol());
				if (endpoint.protocol().family() == AF_INET6)
					socket.set_option(boost::asio::ip::v6_only(true));
				boost::asio::socket_base::reuse_address option(true);
				socket.set_option(option);
				int value = 1;
				::setsockopt(socket.native_handle(), SOL_IP, IP_TRANSPARENT, &value, sizeof(value));
				socket.bind(endpoint);
				socket.listen();
			} catch (const gh::system_error &e) {
				BOOST_LOG_TRIVIAL(error) << "endpiont " << endpoint << " start failed: " << e.what();
				return;
			}
			loop();
		}

	private:
		void loop() {
			auto c = std::make_shared<boost::asio::ip::tcp::socket>(io_service);
			socket.async_accept(*c, [me = shared_from_this(), this, c](const gh::error_code& ec) {
				if (!ec) {
					auto f = std::make_shared<forwarder>(io_service, c);
					f->start();
					me->loop();
				} else {
					BOOST_LOG_TRIVIAL(error) << "acceptor(" << me->socket.local_endpoint() << ") accept error: " << ec.message();
				}
			});
		}

		boost::asio::io_service &io_service;
		boost::asio::ip::tcp::acceptor socket;
};

int main (int ac, char **av) {
	po::options_description desc("Options");
	desc.add_options()
	("help,h", "print this message")
	("syslog", po::bool_switch()->default_value(false), "use syslog instead of stdout")
	("listen,l", po::value<std::vector<std::string> >()->composing(), "listening ports")
	;

	po::variables_map vm;
	try {
		po::store(po::command_line_parser(ac, av).options(desc).run(), vm);
		po::notify(vm);
	} catch (const po::error &ex) {
		std::cerr << ex.what() << std::endl;
		return 1;
	}

	if (vm.count("help") || !vm.count("listen")) {
		std::cerr << "Usage: " << av[0] << " startlua" << std::endl;
		std::cerr << std::endl;
		std::cerr << desc << std::endl;
		return 1;
	}

	if (vm["syslog"].as<bool>()) {
#if defined(HAVE_BOOST_LOG)
		boost::shared_ptr<boost::log::sinks::syslog_backend> backend(new boost::log::sinks::syslog_backend(
				boost::log::keywords::facility = boost::log::sinks::syslog::user,
				boost::log::keywords::use_impl = boost::log::sinks::syslog::native));
		boost::log::core::get()->add_sink(boost::make_shared<boost::log::sinks::synchronous_sink<boost::log::sinks::syslog_backend> >(backend));
#else
		Logger::use_syslog = true;
#endif
	}

	struct rlimit nofile;
	nofile.rlim_cur = 65536;
	nofile.rlim_max = 65536;
	int rc = setrlimit(RLIMIT_NOFILE, &nofile);
	if (rc < 0) {
		char e[200];
		BOOST_LOG_TRIVIAL(warning) << "setrlimit: " << strerror_r(errno, e, sizeof(e));
	}

	boost::asio::io_service io_service;

	boost::asio::signal_set signals(io_service, SIGINT, SIGTERM, SIGPIPE);
	signals.async_wait([&io_service](const gh::error_code& ec, int signal_number) {
		if (!ec) {
			switch (signal_number) {
				case SIGINT:
				case SIGTERM:
					io_service.stop();
					break;
				case SIGHUP:
				case SIGPIPE:
					break;
			}
		} else {
			BOOST_LOG_TRIVIAL(error) << "Sighandler error: " << ec.message();
		}
	});

	auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(io_service);
	for (auto &end : vm["listen"].as<std::vector<std::string>>()) {
		auto handler = [&io_service, resolver, &end](const gh::error_code& ec, boost::asio::ip::tcp::resolver::iterator iterator) {
			if (!ec) {
				for (decltype(iterator) iend; iterator != iend; ++iterator) {
					auto endpoint = iterator->endpoint();
					if (endpoint.address() == boost::asio::ip::address_v4::any() || endpoint.address() == boost::asio::ip::address_v6::any()) {
						BOOST_LOG_TRIVIAL(warning) << "Ignoring ANY address: " << endpoint << end;
						continue;
					}
					BOOST_LOG_TRIVIAL(info) << "Listining on port: " << endpoint << " from " << end;
					std::make_shared<listener>(io_service)->start(endpoint);
				}
			} else {
				BOOST_LOG_TRIVIAL(error) << "Error resolve endpoint(" << end << "): " << ec.message();
			}
		};

		auto s = end.rfind(':');
		if (s == std::string::npos) {
			resolver->async_resolve({end}, handler);
		} else {
			resolver->async_resolve({end.substr(0, s), end.substr(s+1)}, handler);
		}
	}

	io_service.run();

#if defined(HAVE_BOOST_LOG)
	boost::log::core::get()->remove_all_sinks();
#endif

	return 0;
}
