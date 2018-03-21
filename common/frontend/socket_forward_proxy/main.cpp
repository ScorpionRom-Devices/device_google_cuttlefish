/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <glog/logging.h>
#include <gflags/gflags.h>

#include <unistd.h>

#include "common/libs/fs/shared_fd.h"
#include "common/vsoc/lib/socket_forward_region_view.h"

#ifdef CUTTLEFISH_HOST
#include "host/libs/config/host_config.h"
#endif

using vsoc::socket_forward::Packet;
using vsoc::socket_forward::SocketForwardRegionView;

#ifdef CUTTLEFISH_HOST
DEFINE_string(ports, "",
              "Comma-separated list of ports from which to forward TCP "
              "connections.");
#endif

namespace {
// Sends packets, Shutdown(SHUT_WR) on destruction
class SocketSender {
 public:
  explicit SocketSender(cvd::SharedFD socket) : socket_{std::move(socket)} {}

  SocketSender(SocketSender&&) = default;
  SocketSender& operator=(SocketSender&&) = default;

  SocketSender(const SocketSender&&) = delete;
  SocketSender& operator=(const SocketSender&) = delete;

  ~SocketSender() {
    if (socket_.operator->()) {  // check that socket_ was not moved-from
      socket_->Shutdown(SHUT_WR);
    }
  }

  ssize_t SendAll(const Packet& packet) {
    ssize_t written{};
    while (written < static_cast<ssize_t>(packet.payload_length())) {
      if (!socket_->IsOpen()) {
        return -1;
      }
      auto just_written =
          socket_->Send(packet.payload() + written,
                        packet.payload_length() - written, MSG_NOSIGNAL);
      if (just_written <= 0) {
        LOG(INFO) << "Couldn't write to client: "
                  << strerror(socket_->GetErrno());
        return just_written;
      }
      written += just_written;
    }
    return written;
  }

 private:
  cvd::SharedFD socket_;
};

class SocketReceiver {
 public:
  explicit SocketReceiver(cvd::SharedFD socket) : socket_{std::move(socket)} {}

  SocketReceiver(SocketReceiver&&) = default;
  SocketReceiver& operator=(SocketReceiver&&) = default;

  SocketReceiver(const SocketReceiver&&) = delete;
  SocketReceiver& operator=(const SocketReceiver&) = delete;

  // *packet will be empty if Read returns 0 or error
  void Recv(Packet* packet) {
    auto size = socket_->Read(packet->payload(), sizeof packet->payload());
    if (size < 0) {
      size = 0;
    }
    packet->set_payload_length(size);
  }

 private:
  cvd::SharedFD socket_;
};

void SocketToShm(SocketReceiver socket_receiver,
                 SocketForwardRegionView::Sender shm_sender) {
  auto packet = Packet::MakeData();
  while (true) {
    socket_receiver.Recv(&packet);
    if (packet.empty()) {
      break;
    }
    if (!shm_sender.Send(packet)) {
      break;
    }
  }
  LOG(INFO) << "Socket to shm exiting";
}

void ShmToSocket(SocketSender socket_sender,
                 SocketForwardRegionView::Receiver shm_receiver) {
  Packet packet{};
  while (true) {
    shm_receiver.Recv(&packet);
    if (packet.IsEnd()) {
      break;
    }
    if (socket_sender.SendAll(packet) < 0) {
      break;
    }
  }
  LOG(INFO) << "Shm to socket exiting";
}

// One thread for reading from shm and writing into a socket.
// One thread for reading from a socket and writing into shm.
void LaunchWorkers(std::pair<SocketForwardRegionView::Sender,
                             SocketForwardRegionView::Receiver>
                       conn,
                   cvd::SharedFD socket) {
  // TODO create the SocketSender/Receivers in their respective threads?
  std::thread threads[] = {
      std::thread(SocketToShm, SocketReceiver{socket}, std::move(conn.first)),
      std::thread(ShmToSocket, SocketSender{socket}, std::move(conn.second))};
  for (auto&& t : threads) {
    t.detach();
  }
}

#ifdef CUTTLEFISH_HOST
[[noreturn]] void host_impl(SocketForwardRegionView* shm,
                            std::vector<int> ports, std::size_t index) {
  // launch a worker for the following port before handling the current port.
  // recursion (instead of a loop) removes the need fore any join() or having
  // the main thread do no work.
  if (index + 1 < ports.size()) {
    std::thread(host_impl, shm, ports, index + 1).detach();
  }
  auto port = ports[index];
  LOG(INFO) << "starting server on " << port;
  auto server = cvd::SharedFD::SocketLocalServer(port, SOCK_STREAM);
  CHECK(server->IsOpen()) << "Could not start server on port " << port;
  while (true) {
    auto client_socket = cvd::SharedFD::Accept(*server);
    CHECK(client_socket->IsOpen()) << "error creating client socket";
    LOG(INFO) << "client socket accepted";
    auto conn = shm->OpenConnection(port);
    LOG(INFO) << "shm connection opened";
    LaunchWorkers(std::move(conn), std::move(client_socket));
  }
}

[[noreturn]] void host(SocketForwardRegionView* shm,
                           std::vector<int> ports) {
  CHECK(!ports.empty());
  host_impl(shm, ports, 0);
}

std::vector<int> ParsePortsList(const std::string& ports_flag) {
  std::istringstream ports_stream{ports_flag};
  std::vector<int> ports;
  std::string port_str{};
  while (std::getline(ports_stream, port_str, ',')) {
    ports.push_back(std::stoi(port_str));
  }
  return ports;
}

#else
cvd::SharedFD OpenSocketConnection(int port) {
  while (true) {
    auto sock = cvd::SharedFD::SocketLocalClient(port, SOCK_STREAM);
    if (sock->IsOpen()) {
      return sock;
    }
    LOG(WARNING) << "could not connect on port " << port
                 << ". sleeping for 1 second";
    sleep(1);
  }
}

[[noreturn]] void guest(SocketForwardRegionView* shm) {
  LOG(INFO) << "Starting guest mainloop";
  while (true) {
    auto conn = shm->AcceptConnection();
    LOG(INFO) << "shm connection accepted";
    auto sock = OpenSocketConnection(conn.first.port());
    CHECK(sock->IsOpen());
    LOG(INFO) << "socket opened to " << conn.first.port();
    LaunchWorkers(std::move(conn), std::move(sock));
  }
}
#endif

SocketForwardRegionView* GetShm() {
  auto shm = SocketForwardRegionView::GetInstance(
#ifdef CUTTLEFISH_HOST
      vsoc::GetDomain().c_str()
#endif
  );
  if (!shm) {
    LOG(FATAL) << "Could not open SHM. Aborting.";
  }
  shm->CleanUpPreviousConnections();
  return shm;
}

// makes sure we're running as root on the guest, no-op on the host
void assert_correct_user() {
#ifndef CUTTLEFISH_HOST
  CHECK_EQ(getuid(), 0u) << "must run as root!";
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  assert_correct_user();

  auto shm = GetShm();
  auto worker = shm->StartWorker();

#ifdef CUTTLEFISH_HOST
  CHECK(!FLAGS_ports.empty()) << "Must specify --ports flag";
  host(shm, ParsePortsList(FLAGS_ports));
#else
  guest(shm);
#endif
}
