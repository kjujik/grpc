/*
 *
 * Copyright 2018 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <iostream>
#include <string>
#include <set>

#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "examples/protos/chat.grpc.pb.h"
#else
#include "chat.grpc.pb.h"
#endif

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

// Logic and data behind the server's behavior.
class ServiceDiscoveryImpl final : public chat::ServiceDiscovery::Service {
public:
  Status RegisterClient(ServerContext* context, const chat::RegisterReq* request,
                  chat::RegistrationReply* reply) override {
    std::cout << "RegisterClient" << std::endl;;
    const auto name = request->name();
    std::cout << "New client to register: " << name << std::endl;

    if (availableAddressPool_.size() > 0)
    {
      chat::Client client;
      client.set_name(name);
      client.set_full_address(getAvailableAddress());
      const auto insertion_result = registeredClients_.insert(client);

      if (insertion_result.second)
      {
        std::cout << "Client: " << client.name() << ":" << client.full_address() << " registered." << std::endl;
        reply->set_full_address(client.full_address());
      }
      else
      {
         std::cout << "Client: " << client.name() << ":" << client.full_address() << " already registered." << std::endl;
      }
      reply->set_status(insertion_result.second);
    }
    else
    {
      std::cout << "No available pool address" << std::endl;
      reply->set_status(false);
    }
    
    return Status::OK;
  }

  Status DeregisterClient(ServerContext* context, const chat::DeregisterReq* request,
    chat::RegistrationReply* reply) override {
    std::cout << "DeregisterClient" << std::endl;;
    const auto name = request->name();

    chat::Client client;
    client.set_name(name);
    auto it = registeredClients_.find(client);

    if (it != registeredClients_.end())
    {
      std::cout << "Client: " << name << " deregistered."<< std::endl;
      availableAddressPool_.insert(it->full_address());
      registeredClients_.erase(it);
      reply->set_status(true);
    }
    else
    {
      std::cout << "Nothing to deregister for client " << name << "." << std::endl;
      reply->set_status(false);
    }

    return Status::OK;
  }

    Status GetRegisteredClients(ServerContext* context, const chat::GetRegisteredClientsReq* request,
                  chat::RegisteredClientsReply* reply) override {
      std::cout << "GetRegisteredClients" << std::endl;;
      for (const auto& registeredClient : registeredClients_)
      {
        std::cout << "Client: " << registeredClient.name() << ":" << registeredClient.full_address() << std::endl;
        chat::Client* client = reply->add_client();
        client->set_name(registeredClient.name());
        client->set_full_address(registeredClient.full_address());
      }

    return Status::OK;
  }

private:
  std::string getAvailableAddress()
  {
    if (availableAddressPool_.size() == 0)
      return "";

    std::string availableAddress = *(availableAddressPool_.begin());
    availableAddressPool_.erase(availableAddressPool_.begin());

    return availableAddress;
  }

  struct ClientCmp {
    bool operator() (const chat::Client& a, const chat::Client& b) const {
        return a.name() < b.name();}
  };

  std::set<chat::Client, ClientCmp> registeredClients_;
  std::set<std::string> availableAddressPool_ = {"0.0.0.0:50052", "0.0.0.0:50053",
   "0.0.0.0:50054", "0.0.0.0:50055", "0.0.0.0:50056", "0.0.0.0:50057", "0.0.0.0:50058", "0.0.0.0:50059"};
};

void RunServer() {
  std::string server_address("0.0.0.0:50051");
  ServiceDiscoveryImpl service;

  ServerBuilder builder;
  // Set the default compression algorithm for the server.
  builder.SetDefaultCompressionAlgorithm(GRPC_COMPRESS_GZIP);
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  RunServer();
  return 0;
}
