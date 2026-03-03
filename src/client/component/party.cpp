#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include "party.hpp"
#include "auth.hpp"
#include "network.hpp"
#include "scheduler.hpp"
#include "workshop.hpp"
#include "profile_infos.hpp"

#include "connection_log.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/info_string.hpp>
#include <utils/cryptography.hpp>
#include <utils/concurrency.hpp>

namespace party
{
	namespace
	{
		std::atomic_bool is_connecting_to_dedi{false};
		game::netadr_t connect_host{{}, {}, game::NA_BAD, {}};

		constexpr int MAX_WAIT_FOR_SERVER_RETRIES = 30;
		constexpr auto WAIT_FOR_SERVER_RETRY_DELAY = 2s;
		std::atomic_int wait_for_server_retries{0};

		struct server_query
		{
			bool sent{false};
			game::netadr_t host{};
			std::string challenge{};
			query_callback callback{};
			std::chrono::high_resolution_clock::time_point query_time{};
		};

		utils::concurrency::container<std::vector<server_query>>& get_server_queries()
		{
			static utils::concurrency::container<std::vector<server_query>> server_queries;
			return server_queries;
		}

		void connect_to_lobby(const game::netadr_t& addr, const std::string& mapname, const std::string& gamemode,
		                      const std::string& usermap_id, const std::string& mod_id)
		{
			connection_log::log("connect_to_lobby: map=%s gametype=%s usermap=%s mod=%s addr=%u:%u",
			                    mapname.data(), gamemode.data(), usermap_id.data(), mod_id.data(),
			                    addr.addr, static_cast<unsigned>(addr.port));

			auth::clear_stored_guids();

			workshop::setup_same_mod_as_host(usermap_id, mod_id);

			game::XSESSION_INFO info{};
			connection_log::log("connect_to_lobby: calling CL_ConnectFromLobby");
			game::CL_ConnectFromLobby(0, &info, &addr, 1, 0, mapname.data(), gamemode.data(), usermap_id.data());
			connection_log::log("connect_to_lobby: CL_ConnectFromLobby returned");
		}

		void launch_mode(const game::eModes mode)
		{
			scheduler::once([=]
			{
				const auto local_client = *reinterpret_cast<DWORD*>(0x14342155C_g);
				const auto current_mode = game::Com_SessionMode_GetMode();
				game::Com_SwitchMode(local_client, static_cast<game::eModes>(current_mode), mode, 6);
			}, scheduler::main);
		}

		void connect_to_lobby_with_mode(const game::netadr_t& addr, const game::eModes mode, const std::string& mapname,
		                                const std::string& gametype, const std::string& usermap_id,
		                                const std::string& mod_id,
		                                const bool was_retried = false)
		{
			connection_log::log("connect_to_lobby_with_mode: mode=%d mapname=%s gametype=%s was_retried=%d",
			                    static_cast<int>(mode), mapname.data(), gametype.data(), was_retried);

			if (game::Com_SessionMode_IsMode(mode))
			{
				connection_log::log("connect_to_lobby_with_mode: already in correct mode, connecting directly");
				connect_to_lobby(addr, mapname, gametype, usermap_id, mod_id);
				return;
			}

			if (!was_retried)
			{
				connection_log::log("connect_to_lobby_with_mode: wrong mode, launching mode %d and retrying in 5s", static_cast<int>(mode));
				scheduler::once([=]
				{
					connect_to_lobby_with_mode(addr, mode, mapname, gametype, usermap_id, mod_id, true);
				}, scheduler::main, 5s);

				launch_mode(mode);
			}
			else
			{
				connection_log::log("connect_to_lobby_with_mode: FAILED - was_retried=true but still wrong mode");
			}
		}

		game::LobbyMainMode convert_mode(const game::eModes mode)
		{
			switch (mode)
			{
			case game::MODE_CAMPAIGN:
				return game::LOBBY_MAINMODE_CP;
			case game::MODE_MULTIPLAYER:
				return game::LOBBY_MAINMODE_MP;
			case game::MODE_ZOMBIES:
				return game::LOBBY_MAINMODE_ZM;
			default:
				return game::LOBBY_MAINMODE_INVALID;
			}
		}

		void connect_to_session(const game::netadr_t& addr, const std::string& hostname, const uint64_t xuid,
		                        const game::eModes mode)
		{
			connection_log::log("connect_to_session: host=%s xuid=%llX mode=%d addr=%u:%u",
			                    hostname.data(), xuid, static_cast<int>(mode),
			                    addr.addr, static_cast<unsigned>(addr.port));

			const auto LobbyJoin_Begin = reinterpret_cast<bool(*)(int actionId, game::ControllerIndex_t controllerIndex,
			                                                      game::LobbyType sourceLobbyType,
			                                                      game::LobbyType targetLobbyType)>(0x141ED94D0_g);

			if (!LobbyJoin_Begin(0, game::CONTROLLER_INDEX_FIRST, game::LOBBY_TYPE_PRIVATE, game::LOBBY_TYPE_PRIVATE))
			{
				connection_log::log("connect_to_session: LobbyJoin_Begin FAILED");
				return;
			}
			connection_log::log("connect_to_session: LobbyJoin_Begin succeeded");

			auto& join = *game::s_join;

			auto& host = join.hostList[0];
			memset(&host, 0, sizeof(host));

			host.info.netAdr = addr;
			host.info.xuid = xuid;
			utils::string::copy(host.info.name, hostname.data());

			host.lobbyType = game::LOBBY_TYPE_PRIVATE;
			host.lobbyParams.networkMode = game::LOBBY_NETWORKMODE_LIVE;
			host.lobbyParams.mainMode = convert_mode(mode);

			host.retryCount = 0;
			host.retryTime = game::Sys_Milliseconds();

			join.potentialHost = host;
			join.hostCount = 1;
			join.processedCount = 1;
			join.state = game::JOIN_SOURCE_STATE_ASSOCIATING;
			join.startTime = game::Sys_Milliseconds();

			/*join.targetLobbyType = game::LOBBY_TYPE_PRIVATE;
			join.sourceLobbyType = game::LOBBY_TYPE_PRIVATE;
			join.controllerIndex = game::CONTROLLER_INDEX_FIRST;
			join.joinType = game::JOIN_TYPE_NORMAL;
			join.joinResult = game::JOIN_RESULT_INVALID;
			join.isFinalized = false;*/

			// LobbyJoinSource_Finalize
			join.isFinalized = true;
		}

		void handle_connect_query_response(const bool success, const game::netadr_t& target,
		                                   const utils::info_string& info, uint32_t ping)
		{
			connection_log::log("handle_connect_query_response: success=%d ping=%ums addr=%u:%u",
			                    success, ping, target.addr, static_cast<unsigned>(target.port));

			if (!success)
			{
				connection_log::log("handle_connect_query_response: query FAILED (timeout or no response)");
				return;
			}

			is_connecting_to_dedi = info.get("dedicated") == "1";
			connection_log::log("handle_connect_query_response: dedicated=%s", info.get("dedicated").data());

			if (atoi(info.get("protocol").data()) != PROTOCOL)
			{
				connection_log::log("handle_connect_query_response: REJECTED - invalid protocol %s (expected %d)",
				                    info.get("protocol").data(), PROTOCOL);
				const auto str = "Invalid protocol.";
				printf("%s\n", str);
				return;
			}

			const auto sub_protocol = atoi(info.get("sub_protocol").data());
			if (sub_protocol != SUB_PROTOCOL && sub_protocol != (SUB_PROTOCOL - 1))
			{
				connection_log::log("handle_connect_query_response: REJECTED - invalid sub_protocol %d (expected %d)",
				                    sub_protocol, SUB_PROTOCOL);
				const auto str = "Invalid sub-protocol.";
				printf("%s\n", str);
				return;
			}

			const auto gamename = info.get("gamename");
			if (gamename != "T7"s)
			{
				connection_log::log("handle_connect_query_response: REJECTED - invalid gamename '%s'", gamename.data());
				const auto str = "Invalid gamename.";
				printf("%s\n", str);
				return;
			}

			const auto mapname = info.get("mapname");
			if (mapname.empty())
			{
				connection_log::log("handle_connect_query_response: REJECTED - empty mapname");
				const auto str = "Invalid map.";
				printf("%s\n", str);
				return;
			}

			const auto gametype = info.get("gametype");
			if (gametype.empty())
			{
				connection_log::log("handle_connect_query_response: REJECTED - empty gametype");
				const auto str = "Invalid gametype.";
				printf("%s\n", str);
				return;
			}

			const auto sv_running = info.get("sv_running");
			const auto host_not_ready = sv_running != "1" || mapname == "core_frontend";
			if (host_not_ready)
			{
				const auto retry = wait_for_server_retries.fetch_add(1);
				if (retry < MAX_WAIT_FOR_SERVER_RETRIES)
				{
					connection_log::log("handle_connect_query_response: host not ready (sv_running=%s, map=%s), retry %d/%d in 2s",
					                    sv_running.data(), mapname.data(), retry + 1, MAX_WAIT_FOR_SERVER_RETRIES);
					printf("Waiting for host to load the map... (%d/%d)\n", retry + 1, MAX_WAIT_FOR_SERVER_RETRIES);

					scheduler::once([=]
					{
						query_server(connect_host, handle_connect_query_response);
					}, scheduler::async, WAIT_FOR_SERVER_RETRY_DELAY);
					return;
				}

				connection_log::log("handle_connect_query_response: REJECTED - host not ready after %d retries (sv_running=%s, map=%s)",
				                    MAX_WAIT_FOR_SERVER_RETRIES, sv_running.data(), mapname.data());
				printf("Host did not load the map in time.\n");
				wait_for_server_retries = 0;
				return;
			}

			wait_for_server_retries = 0;

			const auto mod_id = info.get("modId");
			const auto workshop_id = info.get("workshop_id");

			const auto playmode = info.get("playmode");
			const auto mode = static_cast<game::eModes>(std::atoi(playmode.data()));

			connection_log::log("handle_connect_query_response: ACCEPTED - map=%s gametype=%s playmode=%s mode=%d mod=%s workshop=%s sv_running=%s clients=%s",
			                    mapname.data(), gametype.data(), playmode.data(), static_cast<int>(mode),
			                    mod_id.data(), workshop_id.data(),
			                    sv_running.data(), info.get("clients").data());

			scheduler::once([=]
			{
				const auto usermap_id = workshop::get_usermap_publisher_id(mapname);

				connection_log::log("handle_connect_query_response [main thread]: usermap_id=%s", usermap_id.data());

				if (workshop::check_valid_usermap_id(mapname, usermap_id, workshop_id) &&
					workshop::check_valid_mod_id(mod_id, workshop_id))
				{
					if (is_connecting_to_dedi)
					{
						connection_log::log("handle_connect_query_response: setting game mode to MATCHMAKING_PLAYLIST (dedicated)");
						game::Com_SessionMode_SetGameMode(game::MODE_GAME_MATCHMAKING_PLAYLIST);
					}

					connection_log::log("handle_connect_query_response: proceeding to connect_to_lobby_with_mode");
					connect_to_lobby_with_mode(target, mode, mapname, gametype, usermap_id, mod_id);
				}
				else
				{
					connection_log::log("handle_connect_query_response: REJECTED - workshop/mod validation failed");
				}
			}, scheduler::main);
		}

		void connect_stub(const char* address)
		{
			connection_log::log("connect_stub: address='%s'", address ? address : "(null)");

			if (address)
			{
				const auto target = network::address_from_string(address);
				if (target.type == game::NA_BAD)
				{
					connection_log::log("connect_stub: FAILED - address resolved to NA_BAD");
					return;
				}

				connection_log::log("connect_stub: resolved to type=%d addr=%u port=%u",
				                    static_cast<int>(target.type), target.addr, static_cast<unsigned>(target.port));
				connect_host = target;
			}

			connection_log::log("connect_stub: clearing profile infos and querying server");
			profile_infos::clear_profile_infos();
			query_server(connect_host, handle_connect_query_response);
		}

		void send_server_query(server_query& query)
		{
			query.sent = true;
			query.query_time = std::chrono::high_resolution_clock::now();
			query.challenge = utils::cryptography::random::get_challenge();

			connection_log::log("send_server_query: sending getInfo to %u:%u challenge=%s",
			                    query.host.addr, static_cast<unsigned>(query.host.port),
			                    query.challenge.data());

			network::send(query.host, "getInfo", query.challenge);
		}

		void handle_info_response(const game::netadr_t& target, const network::data_view& data)
		{
			connection_log::log("handle_info_response: received from %u:%u data_size=%zu",
			                    target.addr, static_cast<unsigned>(target.port), data.size());

			bool found_query = false;
			server_query query{};

			const utils::info_string info{data};

			get_server_queries().access([&](std::vector<server_query>& server_queries)
			{
				connection_log::log("handle_info_response: checking %zu pending queries", server_queries.size());
				for (auto i = server_queries.begin(); i != server_queries.end(); ++i)
				{
					if (i->host == target && i->challenge == info.get("challenge"))
					{
						found_query = true;
						query = std::move(*i);
						i = server_queries.erase(i);
						break;
					}
				}
			});

			if (found_query)
			{
				const auto ping = std::chrono::high_resolution_clock::now() - query.query_time;
				const auto ping_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ping).count();

				connection_log::log("handle_info_response: matched query, ping=%lldms, calling callback", ping_ms);
				query.callback(true, query.host, info, static_cast<uint32_t>(ping_ms));
			}
			else
			{
				connection_log::log("handle_info_response: NO matching query found (stale or unknown response)");
			}
		}

		void cleanup_queried_servers()
		{
			std::vector<server_query> removed_queries{};

			get_server_queries().access([&](std::vector<server_query>& server_queries)
			{
				size_t sent_queries = 0;

				const auto now = std::chrono::high_resolution_clock::now();
				for (auto i = server_queries.begin(); i != server_queries.end();)
				{
					if (!i->sent)
					{
						if (++sent_queries < 40)
						{
							send_server_query(*i);
						}

						++i;
						continue;
					}

					if ((now - i->query_time) < 1s)
					{
						++i;
						continue;
					}

					removed_queries.emplace_back(std::move(*i));
					i = server_queries.erase(i);
				}
			});

			const utils::info_string empty{};
			for (const auto& query : removed_queries)
			{
				connection_log::log("cleanup_queried_servers: query TIMED OUT for %u:%u",
				                    query.host.addr, static_cast<unsigned>(query.host.port));
				query.callback(false, query.host, empty, 0);
			}
		}
	}

	void query_server(const game::netadr_t& host, query_callback callback)
	{
		server_query query{};
		query.sent = false;
		query.host = host;
		query.callback = std::move(callback);

		get_server_queries().access([&](std::vector<server_query>& server_queries)
		{
			server_queries.emplace_back(std::move(query));
		});
	}

	game::netadr_t get_connected_server()
	{
		constexpr auto local_client_num = 0ull;
		const auto address = *reinterpret_cast<uint64_t*>(0x1453D8BB8_g) + (0x25780 * local_client_num) + 0x10;
		return *reinterpret_cast<game::netadr_t*>(address);
	}

	bool is_host(const game::netadr_t& addr)
	{
		return get_connected_server() == addr || connect_host == addr;
	}

	struct component final : client_component
	{
		void post_unpack() override
		{
			utils::hook::jump(0x141EE5FE0_g, &connect_stub);

			network::on("infoResponse", handle_info_response);
			scheduler::loop(cleanup_queried_servers, scheduler::async, 100ms);
		}

		void pre_destroy() override
		{
			get_server_queries().access([](std::vector<server_query>& s)
			{
				s = {};
			});
		}
	};
}

REGISTER_COMPONENT(party::component)
