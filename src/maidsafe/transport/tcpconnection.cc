/* Copyright (c) 2010 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    * Neither the name of the maidsafe.net limited nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <maidsafe/transport/tcptransport.h>
#include <maidsafe/transport/tcpconnection.h>
#include <maidsafe/base/log.h>

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <google/protobuf/descriptor.h>

#include <algorithm>
#include <vector>

namespace asio = boost::asio;
namespace bs = boost::system;
namespace ip = asio::ip;
namespace pt = boost::posix_time;

namespace transport {

TcpConnection::TcpConnection(TcpTransport *tcp_transport,
                             ip::tcp::endpoint const &remote)
  : transport_(tcp_transport),
    socket_(*transport_->asio_service_),
    timer_(*transport_->asio_service_),
    remote_endpoint_(remote),
    buffer_(),
    timeout_for_response_(kDefaultInitialTimeout) {}

TcpConnection::~TcpConnection() {}

void TcpConnection::Close() {
  socket_.close();
  timer_.cancel();
  transport_->RemoveConnection(shared_from_this());
}

ip::tcp::socket &TcpConnection::Socket() {
  return socket_;
}

void TcpConnection::StartTimeout(const Timeout &timeout) {
  timer_.expires_from_now(timeout);
  timer_.async_wait(boost::bind(&TcpConnection::HandleTimeout,
                                shared_from_this(), _1));
}

void TcpConnection::StartReceiving() {
  // Start by receiving the message size.
  // socket_.async_receive(...);
  buffer_.Allocate(sizeof(DataSize));
  asio::async_read(socket_, asio::buffer(buffer_.Data(), buffer_.Size()),
                   boost::bind(&TcpConnection::HandleSize,
                               shared_from_this(), _1));
  StartTimeout(timeout_for_response_);
}

void TcpConnection::HandleTimeout(boost::system::error_code const& ec) {
  if (ec)
    return;
  socket_.close();
}

void TcpConnection::HandleSize(boost::system::error_code const& ec) {
  // If the socket is closed, it means the timeout has been triggered.
  if (!socket_.is_open()) {
    (*transport_->on_error_)(kReceiveTimeout);
    return Close();
  }

  if (ec) {
    (*transport_->on_error_)(kReceiveFailure);
    return Close();
  }

  DataSize size = *reinterpret_cast<DataSize*>(buffer_.Data());
  buffer_.Allocate(size);

  asio::async_read(socket_, asio::buffer(buffer_.Data(), buffer_.Size()),
                   boost::bind(&TcpConnection::HandleRead,
                               shared_from_this(), _1));
}

void TcpConnection::HandleRead(boost::system::error_code const& ec) {
  // If the socket is closed, it means the timeout has been triggered.
  if (!socket_.is_open()) {
    (*transport_->on_error_)(kReceiveTimeout);
    return Close();
  }

  if (ec) {
    (*transport_->on_error_)(kReceiveFailure);
    return Close();
  }

  std::string data(buffer_.Data(), buffer_.Size());
  timer_.cancel();

  DispatchMessage(data);
}

void TcpConnection::DispatchMessage(const std::string &data) {
  // Signal message received and send response if applicable
  std::string response;
  Timeout response_timeout(kImmediateTimeout);
  Info info;
  // TODO(Fraser#5#): 2011-01-18 - Add info details.
  (*transport_->on_message_received_)(data, info, &response, &response_timeout);
  if (response.empty())
    return;

  Send(response, response_timeout, true);
}

void TcpConnection::Send(const std::string &data,
                         const Timeout &timeout,
                         bool is_response) {
  // Serialize message to internal buffer
  DataSize msg_size = data.size();
  if (msg_size > static_cast<size_t>(kMaxTransportMessageSize)) {
    DLOG(ERROR) << "Data size " << msg_size << " bytes (exceeds limit of "
                << kMaxTransportMessageSize << ")" << std::endl;
    (*transport_->on_error_)(kMessageSizeTooLarge);
    return;
  }

  buffer_.Allocate(msg_size + sizeof(DataSize));
  *reinterpret_cast<DataSize*>(buffer_.Data()) = msg_size;
  *reinterpret_cast<std::string*>(buffer_.Data() + sizeof(DataSize)) = data;

  // TODO(Fraser#5#): 2011-01-18 - Check timeout logic
  timeout_for_response_ = timeout;
  if (is_response) {
    assert(socket_.is_open());
//    timeout_for_response_ = kImmediateTimeout;
    Timeout tm_out(std::max(static_cast<Timeout>(msg_size * kTimeoutFactor),
                            kMinTimeout));
    StartTimeout(tm_out);
    asio::async_write(socket_, asio::buffer(buffer_.Data(), buffer_.Size()),
                      boost::bind(&TcpConnection::HandleWrite,
                                  shared_from_this(), _1));
  } else {
    assert(!socket_.is_open());
    StartTimeout(kDefaultInitialTimeout);
    socket_.async_connect(remote_endpoint_,
                          boost::bind(&TcpConnection::HandleConnect,
                                      shared_from_this(), _1));
  }
}

void TcpConnection::HandleConnect(const bs::error_code &ec) {
  // If the socket is closed, it means the timeout has been triggered.
  if (!socket_.is_open()) {
    (*transport_->on_error_)(kSendTimeout);
    return Close();
  }

  if (ec) {
    (*transport_->on_error_)(kSendFailure);
    return Close();
  }

  Timeout tm_out(std::max(static_cast<Timeout>(buffer_.Size() * kTimeoutFactor),
                          kMinTimeout));
  StartTimeout(tm_out);

  asio::async_write(socket_, asio::buffer(buffer_.Data(), buffer_.Size()),
                    boost::bind(&TcpConnection::HandleWrite,
                                shared_from_this(), _1));
}

void TcpConnection::HandleWrite(const bs::error_code &ec) {
  // If the socket is closed, it means the timeout has been triggered.
  if (!socket_.is_open()) {
    (*transport_->on_error_)(kSendTimeout);
    return Close();
  }

  if (ec) {
    (*transport_->on_error_)(kSendFailure);
    return Close();
  }

  // Start receiving response
  if (timeout_for_response_ != kImmediateTimeout) {
    StartReceiving();
  } else {
    Close();
  }
}

}  // namespace transport
