#include "Server.hpp"
#include "Client.hpp"
#include "util.hpp"
#include "common/util/container.hpp"
#include "common/extlib/hash-library/md5.h"
#include "common/util/stream.hpp"
#include "Map.hpp"

#include <format.h>
#include <json11.hpp>
#include <SDL_net.h>

namespace cont = common::util::container;

namespace server {

using namespace json11;

void hello_handler(Server *server, Client *client, Json entity) {
    if (entity.is_string()) {
        fmt::print("Entity: {}\n", entity.string_value());
    }
}

void echo_handler(Server *server, Client *client, Json entity) {
    client->send("echo", entity);
}

Server::Server(IPaddress address, unsigned int max_clients,
               std::string map_name)
    : m_logger(stderr, [] { return "SERVER: "; }) {
    m_address = address;
    m_max_clients = max_clients;
    m_map_name = map_name;
    m_socket_set = SDLNet_AllocSocketSet(m_max_clients);

    initSDL();

    if (!(m_socket = SDLNet_TCP_Open(&address))) {
        m_logger.log("[ERR]  Failed to bind to interface {}", address);
        exit(1);
    }
    m_logger.log("[INFO] Bound to interface {}", m_address);

    m_map_hash = map::map_hash(map_name);

    m_logger.log("Map hash: {}", m_map_hash);
    addHandler("hello", hello_handler);
    addHandler("echo", echo_handler);
}

Server::~Server() { m_logger.log("[INFO] Server shut down.\n\n"); }

void Server::initSDL() {
    SDL_version compile_version;
    const SDL_version *link_version = SDLNet_Linked_Version();
    SDL_NET_VERSION(&compile_version);

    m_logger.log("[INFO] Compiled with SDL_net version: {:d}.{:d}.{:d}",
                 compile_version.major, compile_version.minor,
                 compile_version.patch);
    m_logger.log("[INFO] Running with SDL_net version: {:d}.{:d}.{:d}\n",
                 link_version->major, link_version->minor, link_version->patch);
    if (SDL_Init(0) == -1) {
        m_logger.log("[ERR]  SDL_Init: {}", SDL_GetError());
        m_logger.log("[ERR]  Failed to initialize SDL. Quitting "
                     "zordzman-server...\n");
        exit(1);
    }
    if (SDLNet_Init() == -1) {
        m_logger.log("[ERR]  SDLNet_Init: {}\n", SDLNet_GetError());
        m_logger.log("[ERR]  Failed to initialize SDLNet. Quitting"
                     " zordzman-server...\n");
        exit(1);
    }
}

void Server::sendAll(std::string type, Json entity) {
    for (auto &client : m_clients) {
        client.send(type, entity);
    }
}


void Server::addHandler(std::string type,
        void (*handler)(Server *, Client *, Json)) {
    m_handlers[type].push_back(handler);
}


void Server::acceptConnections() {
    while (true) {
        // Returns immediately with NULL if no pending connections
        TCPsocket client_socket = SDLNet_TCP_Accept(m_socket);

        if (!client_socket) {
            break;
        }

        if (m_clients.size() >= m_max_clients) {
            // Perhaps issue some kind of "server full" warning. But how would
            // this be done as the client would be in the PENDING state
            // intially?
            SDLNet_TCP_Close(client_socket);
        } else {
            m_clients.emplace_back(client_socket);
            Json map_hash_ent = Json::object {
                {"name", m_map_name},
                {"hash", m_map_hash},
            };
            m_clients.back().send("map-hash", map_hash_ent);
            SDLNet_TCP_AddSocket(m_socket_set, client_socket);
        }
    }
}

int Server::exec() {
    while (true) {
        acceptConnections();
        SDLNet_CheckSockets(m_socket_set, 1);
        for (auto &client : m_clients) {
            for (auto &message : client.exec()) {
                // We can't use message.has_shape() here because we don't want
                // to make assumptions about the type of the message entity
                if (message.is_object()) {
                    Json type = message["type"];
                    // If the 'type' field doesn't exist then is_string()
                    // is falsey
                    if (type.is_string()) {
                        for (auto &handler : m_handlers[type.string_value()]) {
                            handler(this, &client, message["entity"]);
                        }
                    }
                }
            }
        }
        // Remove disconnected clients
        for (size_t i = 0; i < m_clients.size(); ++i) {
            Client &client = m_clients[i];

            if (client.getState() == Client::Disconnected) {
                SDLNet_TCP_DelSocket(m_socket_set, client.getSocket());
                m_clients.erase(m_clients.begin() + i);
            }
        }
    }

    return 1;
}
} // namespace server
