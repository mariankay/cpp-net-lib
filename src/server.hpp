#pragma once

#include "service_resolver.hpp"
#include "socket.hpp"
#include "thread_pool.hpp"
#include <atomic>
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <array>
#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace netlib {

    struct client_endpoint {
      netlib::socket socket;
      sockaddr addr{};
      socklen_t addr_len = sizeof(sockaddr);
    };

    struct server_response {
      std::vector<uint8_t> answer{};
      bool terminate = false;
    };

    using callback_connect_t = std::function<server_response(client_endpoint)>;
    using callback_recv_t = std::function<server_response(client_endpoint, std::vector<uint8_t>)>;
    using callback_error_t = std::function<void(client_endpoint, std::error_condition)>;

    class server {
    private:
        std::optional<netlib::socket> _listener_sock;
        int32_t _accept_queue_size = 10;
        std::vector<client_endpoint> _clients;
        std::mutex _mutex;
        std::atomic<bool> _server_active = false;
        callback_connect_t _cb_onconnect{};
        callback_recv_t _cb_on_recv{};
        callback_error_t _cb_on_error{};
        std::thread _accept_thread;
        std::thread _processor_thread;
        netlib::thread_pool _thread_pool = netlib::thread_pool::create<1,1>();
        std::map<socket_t, std::atomic<bool>> _busy_map;

        inline void processing_func() {
          while (_server_active) {
            fd_set fdset;
            socket_t highest_fd = 0;
            FD_ZERO(&fdset);
            std::vector<client_endpoint> local_clients;
            {
              std::lock_guard<std::mutex> lock(_mutex);
              local_clients = _clients;
              for (auto &client : local_clients) {
                socket_t fd = client.socket.get_raw().value();
                assert(_busy_map.contains(fd));
                if (_busy_map[fd]) {
                  // this fd is currently being handled by a task in threadpool
                  continue;
                }
                if (highest_fd < fd) {
                  highest_fd = fd;
                }
                FD_SET(fd, &fdset);
              }
            }

            //windows returns WSAEINVAL if we have nothing to pass to select, so we
            //need to filter this previously and elmulate the sleeping
            if (!highest_fd) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            //we want the timeout to be fairly low, so that we avoid situations where
            //we have a new client in _clients, but are not monitoring it yet - that
            //would mean we have a "hardcoded" delay in servicing a new clients packets
            timeval tv{.tv_sec = 0, .tv_usec= 50 * 1000}; //50ms
            int32_t select_res = ::select(highest_fd + 1, &fdset, nullptr, nullptr, &tv);
            if (select_res > 0) {
              std::vector<client_endpoint> client_refs(select_res);
              int32_t index = 0;
              {
                std::lock_guard<std::mutex> lock(_mutex);
                for (auto& client : local_clients) {
                  socket_t fd = client.socket.get_raw().value();
                  if (FD_ISSET(fd, &fdset)){
                    client_refs[index++] = client;
                    assert(_busy_map.contains(fd));
                    _busy_map[fd] = true;
                  }
                }
              }
              assert(index == select_res);
              //add callback tasks to threadpool for processing
              for (auto& client_to_recv : client_refs) {
                _thread_pool.add_task([&](client_endpoint ce){
                  socket_t id = ce.socket.get_raw().value();
                  std::error_condition error = this->handle_client(ce);
                  if ((error) && (_cb_on_error)) {
                      _cb_on_error(ce, error);
                  }
                  std::lock_guard<std::mutex> lock(_mutex);
                  if (_busy_map.contains(id)) {
                    _busy_map[id] = false;
                  }
                }, client_to_recv);
              }

            } else if (select_res == 0) {
              //nothing interesting happened before timeout
            } else {
              //error was returned
              std::cerr << "server select error: " << socket_get_last_error().message() << std::endl;
            }
          }
        }

        inline void accept_func() {
          client_endpoint new_endpoint;
          while (_server_active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            new_endpoint.addr_len = sizeof(addrinfo);
            socket_t status = ::accept(_listener_sock->get_raw().value(), &new_endpoint.addr, &new_endpoint.addr_len);
            if (status != INVALID_SOCKET) {
              new_endpoint.socket.set_raw(status);
              new_endpoint.socket.set_nonblocking(true);
              if (_cb_onconnect) {
                netlib::server_response greeting = _cb_onconnect(new_endpoint);
                if (!greeting.answer.empty()) {
                  ssize_t send_result = ::send(new_endpoint.socket.get_raw().value(),
                                               reinterpret_cast<const char*>(greeting.answer.data()),
                                               greeting.answer.size(),
                                               0);
                  if ((send_result != greeting.answer.size()) && (_cb_on_error)) {
                    std::cout << "server error accept" << std::endl;
                    _cb_on_error(new_endpoint, socket_get_last_error());
                  }
                }
                if (greeting.terminate) {
                  if (_cb_on_error) {
                    _cb_on_error(new_endpoint, std::errc::connection_aborted);
                  }
                  remove_client(new_endpoint);
                  std::cout << "server kicked client accept" << std::endl;
                  continue;
                }
              }
              if (new_endpoint.socket.is_valid()) {
                std::lock_guard<std::mutex> lock(_mutex);
                std::cout << "server added new client, id " << status << std::endl;
                _clients.push_back(new_endpoint);
                _busy_map[status] = false;
              }
            }
          }
        }

        inline std::error_condition handle_client(client_endpoint endpoint) {
          std::vector<uint8_t> total_buffer;
          std::array<uint8_t, 1024> buffer{};
          ssize_t recv_res = 0;
          while (true) {
            recv_res = ::recv(endpoint.socket.get_raw().value(),
                                    reinterpret_cast<char*>(buffer.data()), buffer.size(), 0);
            if (recv_res > 0) {
              total_buffer.insert(total_buffer.end(), buffer.begin(),
                                  buffer.begin() + recv_res);
            } else if (recv_res == 0) {
              std::cout << "server recv_res == 0" << std::endl;
              remove_client(endpoint);
              std::cout << "server kicked client recv 0" << std::endl;
              return std::errc::connection_aborted;
            } else if (recv_res < 0) {
              // error
              std::error_condition recv_error = socket_get_last_error();
              if ((recv_error == std::errc::resource_unavailable_try_again) ||
                  (recv_error == std::errc::operation_would_block)) {
                // no more data, return what we got
                if (_cb_on_recv) {
                  netlib::server_response response =
                      _cb_on_recv(endpoint, total_buffer);
                  if (!response.answer.empty()) {
                    ssize_t send_result = ::send(
                        endpoint.socket.get_raw().value(),
                        reinterpret_cast<const char*>(response.answer.data()), response.answer.size(), 0);
                    if (send_result != response.answer.size()) {
                      return socket_get_last_error();
                    }
                  }
                  if (response.terminate) {
                    remove_client(endpoint);
                    std::cout << "server kicked client because wanted"
                              << std::endl;
                    return std::errc::connection_aborted;
                  }
                }
                return {};
              } else {
                std::cout << "server recv_res == -1" << std::endl;
                return recv_error;
              }
            }
          }
        }

        bool remove_client(client_endpoint& ce) {
          //the remove_if-> erase idiom is perhaps my most hated part about std containers
          std::lock_guard<std::mutex> lock(_mutex);
          std::size_t client_count = _clients.size();
          _busy_map.erase(ce.socket.get_raw().value());
          std::cout << "remove_client, client count at start " << _clients.size() << std::endl;
          std::erase_if(_clients, [&](const client_endpoint& single_endpoint){
            return (ce.socket.get_raw().value() == single_endpoint.socket.get_raw().value());
          });
          std::cout << "removed client, id " << ce.socket.get_raw().value() << std::endl;
          ce.socket.close();
          assert((client_count - _clients.size()) == 1);
          return true;
        }

      public:
        server() {
          netlib::socket::initialize_system();
        }
        virtual ~server() { stop();
        }
        inline std::error_condition create(const std::string& bind_host,
                                    const std::variant<std::string,uint16_t>& service,
                                    AddressFamily address_family,
                                    AddressProtocol address_protocol) {
          if (_listener_sock.has_value()) {
            this->stop();
          }

          const std::string service_string = std::holds_alternative<uint16_t>(service) ? std::to_string(std::get<uint16_t>(service)) : std::get<std::string>(service);
          std::pair<addrinfo*, std::error_condition> addrinfo_result = service_resolver::get_addrinfo(std::nullopt, service_string, address_family, address_protocol, AI_PASSIVE);

          if (addrinfo_result.first == nullptr) {
            return addrinfo_result.second;
          }

          auto close_and_free = [&](){
            this->stop();
            freeaddrinfo(addrinfo_result.first);
          };

          for (addrinfo* res_addrinfo = addrinfo_result.first; res_addrinfo != nullptr; res_addrinfo = res_addrinfo->ai_next) {
            _listener_sock = netlib::socket();
            std::error_condition s_create_error = _listener_sock->create(res_addrinfo->ai_family, res_addrinfo->ai_socktype, res_addrinfo->ai_protocol);
            if (s_create_error) {
              close_and_free();
              continue;
            }
            _listener_sock->set_nonblocking(true); //we want to be able to join
            int32_t res = ::bind(_listener_sock->get_raw().value(), res_addrinfo->ai_addr, res_addrinfo->ai_addrlen);
            if (res < 0) {
              close_and_free();
              continue;
            }
            if (address_protocol == AddressProtocol::TCP) {
              res = ::listen(_listener_sock->get_raw().value(), _accept_queue_size);
              if (res < 0) {
                close_and_free();
                continue;
              }
            }
            //all went well
            break;
          }
          if (_listener_sock) {
            _server_active = true;
            _accept_thread = std::thread(&server::accept_func, this);
            _processor_thread = std::thread(&server::processing_func, this);
            return {};
          }
          return socket_get_last_error();
        }
        inline void register_callback_on_connect(callback_connect_t onconnect) {_cb_onconnect = std::move(onconnect);};
        inline void register_callback_on_recv(callback_recv_t onrecv) {_cb_on_recv = std::move(onrecv);};
        inline void register_callback_on_error(callback_error_t onerror) {_cb_on_error = std::move(onerror);};

        inline void stop(){
          _server_active = false;
          if (_accept_thread.joinable()) {
            _accept_thread.join();
          }
          if (_processor_thread.joinable()){
            _processor_thread.join();
          }
          if (_listener_sock.has_value()) {
            _listener_sock->close();
            _listener_sock.reset();
          }
          std::cout << "server stopped" << std::endl;
        }

        inline std::size_t get_client_count() {
          std::lock_guard<std::mutex> lock(_mutex);
          return _clients.size();
        }
    };
}

