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
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <chrono>

#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "examples/protos/chat.grpc.pb.h"
#else
#include "chat.grpc.pb.h"
#endif

using grpc::Channel;
using grpc::ChannelArguments;
using grpc::ClientContext;
using grpc::Status;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;

void printClient(const chat::Client& client)
{
  std::cout << "-   " << client.name() << ":" << client.full_address() << "  -" << std::endl;
}

template <class T>
void printStlWithClients(const T& container)
{
  std::for_each(std::begin(container), std::end(container), printClient);
}

class ClientServiceDiscovery {
public:
  explicit ClientServiceDiscovery(const std::string& name, std::shared_ptr<Channel> channel) 
  : name_(name), service_discoveryStub_(chat::ServiceDiscovery::NewStub(channel))
   {}

  std::pair<bool, std::string> RegisterClient()
   {
    chat::RegisterReq request;

    request.set_name(name_);

    chat::RegistrationReply reply;

    ClientContext context;
    context.set_compression_algorithm(GRPC_COMPRESS_DEFLATE);

    Status status = service_discoveryStub_->RegisterClient(&context, request, &reply);

    if (status.ok())
    {
      if (reply.status())
      {
        std::cout << "Client registered successfuly, received server address:  " << reply.full_address() << std::endl;
        return std::make_pair(reply.status(), reply.full_address());
      }
      else
      {
        std::cout << "Server found, but registration failed." << std::endl;
        return std::make_pair(reply.status(), "");
      }
      
    }
    else
    {
      std::cout << "Server not found yet" << std::endl;
      return std::make_pair(false, "");;
    }
  }

  bool DeregisterClient() {
    chat::DeregisterReq request;

    request.set_name(name_);

    chat::RegistrationReply reply;

    ClientContext context;
    context.set_compression_algorithm(GRPC_COMPRESS_DEFLATE);

    Status status = service_discoveryStub_->DeregisterClient(&context, request, &reply);

    if (status.ok()) {
      if (reply.status())
      {
        std::cout << "Client deregistered successfuly." << std::endl;
      }
      else
      {
        std::cout << "Client deregistatrion failed." << std::endl;
      }
      return reply.status();
    }
    else
    {
      std::cout << status.error_code() << ": " << status.error_message()
                << std::endl;
      return false;
    }
  }

  std::vector<chat::Client> GetRegisteredClients()
  {
    chat::GetRegisteredClientsReq request;
    chat::RegisteredClientsReply reply;

    ClientContext context;
    context.set_compression_algorithm(GRPC_COMPRESS_DEFLATE);

    Status status = service_discoveryStub_->GetRegisteredClients(&context, request, &reply);
    std::vector<chat::Client> result;
    if (status.ok())
     {
      for (const auto& registeredClient : reply.client())
      {
        if (registeredClient.name() == name_)
          continue;
        std::cout << "Adding client to comunicate: ";
        std::cout << registeredClient.name() << ":" << registeredClient.full_address() << std::endl;

        result.push_back(registeredClient);
      }
    } else {
      std::cout << status.error_code() << ": " << status.error_message()
                << std::endl;
    }
    return result;
  }
private:
  const std::string name_;
  std::unique_ptr<chat::ServiceDiscovery::Stub> service_discoveryStub_;
};

class ClientChat
{
public:
  explicit ClientChat(const std::string& name, std::shared_ptr<Channel> channel) 
  : name_(name), chatStub_(chat::Chat::NewStub(channel))
  { 
  }

  void SendMsg(const std::string& msg)
  {
    chat::MessageReq request;

    request.set_name(name_);
    request.set_message(msg);

    chat::Ack reply;

    ClientContext context;
    context.set_compression_algorithm(GRPC_COMPRESS_DEFLATE);

    Status status = chatStub_->SendMsg(&context, request, &reply);

    if (!status.ok())
    {
      std::cout << status.error_code() << ": " << status.error_message()
                << std::endl;
    }
  }

private:
  const std::string name_;
  std::unique_ptr<chat::Chat::Stub> chatStub_;
};

class ServerChat final : public chat::Chat::Service
{
public:
    explicit ServerChat(std::mutex& cout_mutex) : cout_mutex_(cout_mutex)
    {}

    using Data = std::vector<std::pair<std::string, std::string>>;

    Status SendMsg(ServerContext* context, const chat::MessageReq* request,
                  chat::Ack* reply) override
   {
       cout_mutex_.lock();
       bool newDataAvailable_ = true;
       std::cout << "---------------------NEW MESSAGE-------------------------------" << std::endl;
       std::cout << request->name() << ":" <<  request->message() << std::endl;
       std::cout << "---------------------END OF NEW MESSAGE------------------------" << std::endl;
       cout_mutex_.unlock();

       std::lock_guard<std::mutex> guard(data_mutex_);
       messages_.emplace_back(request->name(), request->message());
       newDataAvailable_ = true;
       return Status::OK;
   }

   bool isDataAvailable() const
   {
    return newDataAvailable_;
   }

   Data flushData()
   {
       std::lock_guard<std::mutex> guard(data_mutex_);
       auto result = std::move(messages_);
       messages_.clear();
       newDataAvailable_ = false;
       return result;
   }

private:
   bool newDataAvailable_ = false;
   Data messages_;
   std::mutex data_mutex_;
   std::mutex& cout_mutex_;
};

class ChatManager {
 public:
  explicit ChatManager(const std::string& name, std::unique_ptr<ClientServiceDiscovery> clientServiceDiscovery)
      : name_(name), clientServiceDiscovery_(std::move(clientServiceDiscovery)) {}
  ~ChatManager()
{
  if (address_ != "")
  {
    clientServiceDiscovery_->DeregisterClient();
    server_threadPtr_.reset(nullptr);
  }
}

enum class GuiContext {EEnd = 0, ERegistrationMenu = 1, EClientsMenu = 2, ETryRegisterMenu = 3};

GuiContext handleRegistartionMenu()
{
  cout_mutex_.lock();
  std::cout << "Choose option:" << std::endl;
  std::cout << "1: list registered clients from server" << std::endl;
  std::cout << "Q: to exit" << std::endl;
  cout_mutex_.unlock();
  std::string input;
  std::getline(std::cin, input);

  if (input == "1")
  {
    return getRegisteredClients();
  }
  else if (input == "2")
  {
    return startChatWithClient();
  }
  else if (input == "Q")
  {
    return GuiContext::EEnd;
  }
  else  
  {
    std::lock_guard<std::mutex> guard(cout_mutex_);
    std::cout << "Unsupported option choosen, try again" << std::endl;
  }

  return GuiContext::ERegistrationMenu;
}

GuiContext getRegisteredClients()
{
  const auto result = clientServiceDiscovery_->GetRegisteredClients();
  if (result.size())
  {
    clientsToComunicate_ = std::move(result);

    cout_mutex_.lock();
    std::cout << "----------------------------------------------------" << std::endl;
    printStlWithClients(clientsToComunicate_);
    std::cout << "----------------------------------------------------" << std::endl;
    cout_mutex_.unlock();
  }
  else
  {
    std::lock_guard<std::mutex> guard(cout_mutex_);
    std::cout << "No other clients registered to server" << std::endl;
  }
  return GuiContext::ERegistrationMenu;
}

GuiContext startChatWithClient()
{
    std::cout << "Pick client to start chat or type \'back\' to back to previous menu" << std::endl;
    cout_mutex_.unlock();
    std::string clientName;
    std::getline(std::cin, clientName);

    if (clientName == "back")
      return GuiContext::ERegistrationMenu;

    if (auto it = std::find_if(clientsToComunicate_.begin(), clientsToComunicate_.end(),
     [&clientName](const chat::Client& client){return client.name() == clientName;}); it != clientsToComunicate_.end())
    {
      ChannelArguments args;
      args.SetCompressionAlgorithm(GRPC_COMPRESS_GZIP);
      clientChat_ = std::make_unique<ClientChat>(name_, grpc::CreateCustomChannel(
        it->full_address(), grpc::InsecureChannelCredentials(), args));
      
      cout_mutex_.lock();
      std::cout << "Send message: " << std::endl;
      cout_mutex_.unlock();
      std::string message;
      std::getline(std::cin, message);

      clientChat_->SendMsg(message);
      return GuiContext::EClientsMenu;
    }
    else
    {
      cout_mutex_.lock();
      std::cout << "Wrong client" << std::endl;
      cout_mutex_.unlock();
      return GuiContext::EClientsMenu;
    }
}

GuiContext tryToRegisterMenu()
{     
  const auto result = clientServiceDiscovery_->RegisterClient();
  if (result.first)
  {
    address_ = result.second;
    server_threadPtr_ = std::make_unique<std::thread>(&ChatManager::startChatServer, this);
    server_threadPtr_->detach();
    return GuiContext::ERegistrationMenu;
  }
  else
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return GuiContext::ETryRegisterMenu;
  }
}

void startChatServer()
{
  if (address_ == "")
    return;

  ServerChat service(cout_mutex_);

  ServerBuilder builder;
  builder.SetDefaultCompressionAlgorithm(GRPC_COMPRESS_GZIP);
  builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  cout_mutex_.lock();
  std::cout << "Chat server listening on " << address_ << std::endl;
  cout_mutex_.unlock();
  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

void start()
{
  std::cout << "Client " << name_ << " is started" << std::endl;
  bool end = true;
  GuiContext guiContext = GuiContext::ETryRegisterMenu;
  while(guiContext != GuiContext::EEnd)
  {
    switch (guiContext)
    {
      case GuiContext::ETryRegisterMenu:
      {
        guiContext = tryToRegisterMenu();
        break;
      }
      case GuiContext::ERegistrationMenu:
      {
        guiContext = handleRegistartionMenu();
        break;
      }
      case GuiContext::EClientsMenu:
      {
        guiContext = handleClientsMenu();
        break;
      }
      default:
        break;
    }
    
  }
}

private:
  std::unique_ptr<ClientServiceDiscovery> clientServiceDiscovery_;
  std::unique_ptr<ClientChat> clientChat_;
  std::unique_ptr<std::thread> server_threadPtr_;

  const std::string name_;
  std::string address_;
  std::vector<chat::Client> clientsToComunicate_;
  std::mutex cout_mutex_;
};

int main(int argc, char** argv) {
  ChannelArguments args;
  args.SetCompressionAlgorithm(GRPC_COMPRESS_GZIP);

  std::cout << "Put name for your client to start " << std::endl;
  std::string name;
  std::getline(std::cin, name);

  ChatManager chatManager(name,
   std::make_unique<ClientServiceDiscovery>(name, grpc::CreateCustomChannel(
    "localhost:50051", grpc::InsecureChannelCredentials(), args)));
  chatManager.start();

  return 0;
}
