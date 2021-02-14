// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bin/driver_manager/driver_runner.h"

#include <lib/fdio/directory.h>
#include <lib/fidl/llcpp/server.h>
#include <zircon/status.h>

#include <sstream>
#include <stack>
#include <unordered_set>

#include <fs/service.h>

#include "src/devices/lib/driver2/start_args.h"
#include "src/devices/lib/log/log.h"

namespace fdata = llcpp::fuchsia::data;
namespace fdf = llcpp::fuchsia::driver::framework;
namespace fio = llcpp::fuchsia::io;
namespace frunner = llcpp::fuchsia::component::runner;
namespace fsys = llcpp::fuchsia::sys2;

using InspectStack = std::stack<std::pair<inspect::Node*, Node*>>;

namespace {

void InspectNode(inspect::Inspector* inspector, InspectStack* stack) {
  std::vector<inspect::Node> roots;
  while (!stack->empty()) {
    // Pop the current root and node to operate on.
    auto [root, node] = stack->top();
    stack->pop();

    // Populate root with data from node.
    if (auto offers = node->offers(); !offers.empty()) {
      std::ostringstream stream;
      for (auto& offer : offers) {
        stream << (stream.tellp() > 0 ? ", " : "") << offer.get();
      }
      root->CreateString("offers", stream.str(), inspector);
    }
    if (auto symbols = node->symbols(); !symbols.empty()) {
      std::ostringstream stream;
      for (auto& symbol : symbols) {
        stream << (stream.tellp() > 0 ? ", " : "") << symbol.name().get();
      }
      root->CreateString("symbols", stream.str(), inspector);
    }

    // Push children of this node onto the stack.
    for (auto& child : node->children()) {
      auto& root_for_child = roots.emplace_back(root->CreateChild(child.name()));
      stack->emplace(&root_for_child, &child);
    }
  }

  // Store all of the roots in the inspector.
  for (auto& root : roots) {
    inspector->emplace(std::move(root));
  }
}

}  // namespace

class EventHandler : public fdf::DriverHost::AsyncEventHandler {
 public:
  EventHandler(DriverHostComponent* component,
               fbl::DoublyLinkedList<std::unique_ptr<DriverHostComponent>>* driver_hosts)
      : component_(component), driver_hosts_(driver_hosts) {}

  void Unbound(fidl::UnbindInfo info) override { driver_hosts_->erase(*component_); }

 private:
  DriverHostComponent* const component_;
  fbl::DoublyLinkedList<std::unique_ptr<DriverHostComponent>>* driver_hosts_;
};

DriverComponent::DriverComponent(fidl::ClientEnd<fio::Directory> exposed_dir,
                                 fidl::ClientEnd<fdf::Driver> driver)
    : exposed_dir_(std::move(exposed_dir)), driver_(std::move(driver)) {}

void DriverComponent::Stop(DriverComponent::StopCompleter::Sync& completer) { driver_.reset(); }

void DriverComponent::Kill(DriverComponent::KillCompleter::Sync& completer) {}

DriverHostComponent::DriverHostComponent(
    fidl::ClientEnd<fdf::DriverHost> driver_host, async_dispatcher_t* dispatcher,
    fbl::DoublyLinkedList<std::unique_ptr<DriverHostComponent>>* driver_hosts)
    : driver_host_(std::move(driver_host), dispatcher,
                   std::make_shared<EventHandler>(this, driver_hosts)) {}

zx::status<fidl::ClientEnd<fdf::Driver>> DriverHostComponent::Start(
    fidl::ClientEnd<fdf::Node> node, fidl::VectorView<fidl::StringView> offers,
    fidl::VectorView<fdf::NodeSymbol> symbols, fidl::StringView url, fdata::Dictionary program,
    fidl::VectorView<frunner::ComponentNamespaceEntry> ns,
    fidl::ServerEnd<fio::Directory> outgoing_dir, fidl::ClientEnd<fio::Directory> exposed_dir) {
  auto endpoints = fidl::CreateEndpoints<fdf::Driver>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }
  auto args = fdf::DriverStartArgs::UnownedBuilder()
                  .set_node(fidl::unowned_ptr(&node))
                  .set_offers(fidl::unowned_ptr(&offers))
                  .set_symbols(fidl::unowned_ptr(&symbols))
                  .set_url(fidl::unowned_ptr(&url))
                  .set_program(fidl::unowned_ptr(&program))
                  .set_ns(fidl::unowned_ptr(&ns))
                  .set_outgoing_dir(fidl::unowned_ptr(&outgoing_dir))
                  .set_exposed_dir(fidl::unowned_ptr(&exposed_dir));
  auto start = driver_host_->Start(args.build(), std::move(endpoints->server));
  if (!start.ok()) {
    auto binary = start_args::program_value(program, "binary").value_or("");
    LOGF(ERROR, "Failed to start driver '%s' in driver host: %s", binary.data(), start.error());
    return zx::error(start.status());
  }
  return zx::ok(std::move(endpoints->client));
}

Node::Node(Node* parent, DriverBinder* driver_binder, async_dispatcher_t* dispatcher,
           std::string_view name, Offers offers, Symbols symbols)
    : parent_(parent),
      driver_binder_(driver_binder),
      dispatcher_(dispatcher),
      name_(std::move(name)),
      offers_(std::move(offers)),
      symbols_(std::move(symbols)) {}

Node::~Node() {
  if (controller_binding_.has_value()) {
    controller_binding_->Unbind();
  }
  if (binding_.has_value()) {
    binding_->Unbind();
  }
}

const std::string& Node::name() const { return name_; }

fidl::VectorView<fidl::StringView> Node::offers() { return fidl::unowned_vec(offers_); }

fidl::VectorView<fdf::NodeSymbol> Node::symbols() { return fidl::unowned_vec(symbols_); }

DriverHostComponent* Node::parent_driver_host() const { return parent_->driver_host_; }

void Node::set_driver_host(DriverHostComponent* driver_host) { driver_host_ = driver_host; }

void Node::set_controller_binding(fidl::ServerBindingRef<fdf::NodeController> controller_binding) {
  controller_binding_ = std::make_optional(std::move(controller_binding));
}

void Node::set_binding(fidl::ServerBindingRef<fdf::Node> binding) {
  binding_ = std::make_optional(std::move(binding));
}

fbl::DoublyLinkedList<std::unique_ptr<Node>>& Node::children() { return children_; }

void Node::Remove() {
  if (parent_ != nullptr) {
    parent_->children_.erase(*this);
  }
}

void Node::Remove(RemoveCompleter::Sync& completer) {
  // When NodeController::Remove() is called, we unbind the Node. This causes
  // the Node binding to then call Node::Remove().
  //
  // We take this approach to avoid a use-after-free, where calling
  // Node::Remove() directly would then cause the the Node binding to do the
  // same, after the Node has already been freed.
  if (binding_.has_value()) {
    binding_->Unbind();
    binding_.reset();
  }
}

void Node::AddChild(fdf::NodeAddArgs args, fidl::ServerEnd<fdf::NodeController> controller,
                    fidl::ServerEnd<fdf::Node> node, AddChildCompleter::Sync& completer) {
  auto name = args.has_name() ? std::move(args.name()) : fidl::StringView();
  Offers offers;
  if (args.has_offers()) {
    offers.reserve(args.offers().count());
    std::unordered_set<std::string_view> names;
    for (auto& offer : args.offers()) {
      auto inserted = names.emplace(offer.data(), offer.size()).second;
      if (!inserted) {
        LOGF(ERROR, "Failed to add Node '%.*s', duplicate offer '%.*s'", name.size(), name.data(),
             offer.size(), offer.data());
        completer.Close(ZX_ERR_INVALID_ARGS);
        return;
      }
      offers.emplace_back(fidl::heap_copy_str(offer));
    }
  }
  Symbols symbols;
  if (args.has_symbols()) {
    symbols.reserve(args.symbols().count());
    std::unordered_set<std::string_view> names;
    for (auto& symbol : args.symbols()) {
      if (!symbol.has_name()) {
        LOGF(ERROR, "Failed to add Node '%.*s', a symbol is missing a name", name.size(),
             name.data());
      }
      if (!symbol.has_address()) {
        LOGF(ERROR, "Failed to add Node '%.*s', symbol '%.*s' is missing an address", name.size(),
             name.data(), symbol.name().size(), symbol.name().data());
      }
      auto inserted = names.emplace(symbol.name().data(), symbol.name().size()).second;
      if (!inserted) {
        LOGF(ERROR, "Failed to add Node '%.*s', duplicate symbol '%.*s'", name.size(), name.data(),
             symbol.name().size(), symbol.name().data());
        completer.Close(ZX_ERR_INVALID_ARGS);
        return;
      }
      symbols.emplace_back(
          fdf::NodeSymbol::Builder(std::make_unique<fdf::NodeSymbol::Frame>())
              .set_name(std::make_unique<fidl::StringView>(fidl::heap_copy_str(symbol.name())))
              .set_address(std::make_unique<zx_vaddr_t>(symbol.address()))
              .build());
    }
  }
  auto child = std::make_unique<Node>(this, driver_binder_, dispatcher_, name.get(),
                                      std::move(offers), std::move(symbols));

  auto bind_controller = fidl::BindServer<fdf::NodeController::Interface>(
      dispatcher_, std::move(controller), child.get());
  if (bind_controller.is_error()) {
    LOGF(ERROR, "Failed to bind channel to NodeController '%.*s': %s", name.size(), name.data(),
         zx_status_get_string(bind_controller.error()));
    completer.Close(bind_controller.error());
    return;
  }
  child->set_controller_binding(bind_controller.take_value());

  if (node.is_valid()) {
    auto bind_node = fidl::BindServer<fdf::Node::Interface>(
        dispatcher_, std::move(node), child.get(),
        [](fdf::Node::Interface* node, auto, auto) { static_cast<Node*>(node)->Remove(); });
    if (bind_node.is_error()) {
      LOGF(ERROR, "Failed to bind channel to Node '%.*s': %s", name.size(), name.data(),
           zx_status_get_string(bind_node.error()));
      completer.Close(bind_node.error());
      return;
    }
    child->set_binding(bind_node.take_value());
  } else {
    auto bind_result = driver_binder_->Bind(child.get(), std::move(args));
    if (bind_result.is_error()) {
      LOGF(ERROR, "Failed to bind driver to Node '%.*s': %s", name.size(), name.data(),
           bind_result.status_string());
      completer.Close(bind_result.status_value());
      return;
    }
  }

  children_.push_back(std::move(child));
}

DriverIndex::DriverIndex(MatchCallback match_callback)
    : match_callback_(std::move(match_callback)) {}

zx::status<MatchResult> DriverIndex::Match(fdf::NodeAddArgs args) {
  return match_callback_(std::move(args));
}

DriverRunner::DriverRunner(fidl::ClientEnd<llcpp::fuchsia::sys2::Realm> realm,
                           DriverIndex* driver_index, inspect::Inspector* inspector,
                           async_dispatcher_t* dispatcher)
    : realm_(std::move(realm), dispatcher),
      driver_index_(driver_index),
      dispatcher_(dispatcher),
      root_node_(nullptr, this, dispatcher, "root", {}, {}) {
  inspector->GetRoot().CreateLazyNode(
      "driver_runner", [this] { return Inspect(); }, inspector);
}

fit::promise<inspect::Inspector> DriverRunner::Inspect() {
  inspect::Inspector inspector;
  auto root = inspector.GetRoot().CreateChild(root_node_.name());
  InspectStack stack{{std::make_pair(&root, &root_node_)}};
  InspectNode(&inspector, &stack);
  inspector.emplace(std::move(root));
  return fit::make_ok_promise(inspector);
}

zx::status<> DriverRunner::PublishComponentRunner(const fbl::RefPtr<fs::PseudoDir>& svc_dir) {
  const auto service = [this](zx::channel request) {
    auto result = fidl::BindServer(dispatcher_, std::move(request), this);
    if (result.is_error()) {
      LOGF(ERROR, "Failed to bind channel to '%s': %s", frunner::ComponentRunner::Name,
           zx_status_get_string(result.error()));
      return result.error();
    }
    return ZX_OK;
  };
  zx_status_t status =
      svc_dir->AddEntry(frunner::ComponentRunner::Name, fbl::MakeRefCounted<fs::Service>(service));
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to add directory entry '%s': %s", frunner::ComponentRunner::Name,
         zx_status_get_string(status));
  }
  return zx::make_status(status);
}

zx::status<> DriverRunner::StartRootDriver(std::string_view name) {
  auto root_name = fidl::unowned_str(name);
  auto args = fdf::NodeAddArgs::UnownedBuilder().set_name(fidl::unowned_ptr(&root_name));
  return Bind(&root_node_, args.build());
}

void DriverRunner::Start(frunner::ComponentStartInfo start_info,
                         fidl::ServerEnd<frunner::ComponentController> controller,
                         StartCompleter::Sync& completer) {
  std::string url(start_info.resolved_url().get());
  auto it = driver_args_.find(url);
  if (it == driver_args_.end()) {
    LOGF(ERROR, "Failed to start driver '%.*s', unknown request for driver", url.size(),
         url.data());
    completer.Close(ZX_ERR_UNAVAILABLE);
    return;
  }
  auto driver_args = std::move(it->second);
  driver_args_.erase(it);
  auto symbols = driver_args.node->symbols();

  // Launch a driver host, or use an existing driver host.
  DriverHostComponent* driver_host;
  if (start_args::program_value(start_info.program(), "colocate").value_or("") == "true") {
    if (driver_args.node == &root_node_) {
      LOGF(ERROR, "Failed to start driver '%.*s', root driver cannot colocate", url.size(),
           url.data());
      completer.Close(ZX_ERR_INVALID_ARGS);
      return;
    }
    driver_host = driver_args.node->parent_driver_host();
  } else {
    // Do not pass symbols across driver hosts.
    symbols.set_count(0);

    auto result = StartDriverHost();
    if (result.is_error()) {
      completer.Close(result.error_value());
      return;
    }
    driver_host = result.value().get();
    driver_hosts_.push_back(std::move(result.value()));
  }
  driver_args.node->set_driver_host(driver_host);

  // Start the driver within the driver host.
  auto endpoints = fidl::CreateEndpoints<fdf::Node>();
  if (endpoints.is_error()) {
    completer.Close(endpoints.status_value());
    return;
  }
  zx::channel exposed_dir(fdio_service_clone(driver_args.exposed_dir.channel().get()));
  if (!exposed_dir.is_valid()) {
    LOGF(ERROR, "Failed to clone exposed directory for driver '%.*s'", url.size(), url.data());
    completer.Close(ZX_ERR_INTERNAL);
    return;
  }
  auto start = driver_host->Start(std::move(endpoints->client), driver_args.node->offers(),
                                  std::move(symbols), std::move(start_info.resolved_url()),
                                  std::move(start_info.program()), std::move(start_info.ns()),
                                  std::move(start_info.outgoing_dir()), std::move(exposed_dir));
  if (start.is_error()) {
    completer.Close(start.error_value());
    return;
  }

  // Create a DriverComponent to manage the driver.
  auto driver = std::make_unique<DriverComponent>(std::move(driver_args.exposed_dir),
                                                  std::move(start.value()));
  auto bind_driver = fidl::BindServer<DriverComponent>(
      dispatcher_, std::move(controller), driver.get(),
      [this](DriverComponent* driver, auto, auto) { drivers_.erase(*driver); });
  if (bind_driver.is_error()) {
    LOGF(ERROR, "Failed to bind channel to ComponentController for driver '%.*s': %s", url.size(),
         url.data(), zx_status_get_string(bind_driver.error()));
    completer.Close(bind_driver.error());
    return;
  }

  // Bind the Node associated with the driver.
  auto bind_node = fidl::BindServer<fdf::Node::Interface>(
      dispatcher_, std::move(endpoints->server), driver_args.node,
      [driver_binding = bind_driver.take_value()](fdf::Node::Interface* node, auto, auto) mutable {
        driver_binding.Unbind();
        static_cast<Node*>(node)->Remove();
      });
  if (bind_node.is_error()) {
    LOGF(ERROR, "Failed to bind channel to Node for driver '%.*s': %s", url.size(), url.data(),
         zx_status_get_string(bind_node.error()));
    completer.Close(bind_node.error());
    return;
  }
  driver_args.node->set_binding(bind_node.take_value());
  drivers_.push_back(std::move(driver));
}

zx::status<> DriverRunner::Bind(Node* node, fdf::NodeAddArgs args) {
  auto match_result = driver_index_->Match(std::move(args));
  if (match_result.is_error()) {
    return match_result.take_error();
  }
  auto match = std::move(match_result.value());
  auto name = "driver-" + std::to_string(NextId());
  auto create_result = CreateComponent(name, match.url, "drivers");
  if (create_result.is_error()) {
    return create_result.take_error();
  }
  driver_args_.emplace(match.url, DriverArgs{std::move(create_result.value()), node});
  return zx::ok();
}

zx::status<std::unique_ptr<DriverHostComponent>> DriverRunner::StartDriverHost() {
  auto name = "driver_host-" + std::to_string(NextId());
  auto create = CreateComponent(name, "fuchsia-boot:///#meta/driver_host2.cm", "driver_hosts");
  if (create.is_error()) {
    return create.take_error();
  }

  auto endpoints = fidl::CreateEndpoints<fdf::DriverHost>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }
  zx_status_t status = fdio_service_connect_at(create->channel().get(), fdf::DriverHost::Name,
                                               endpoints->server.TakeChannel().release());
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to connect to service '%s': %s", fdf::DriverHost::Name,
         zx_status_get_string(status));
    return zx::error(status);
  }

  auto driver_host = std::make_unique<DriverHostComponent>(std::move(endpoints->client),
                                                           dispatcher_, &driver_hosts_);
  return zx::ok(std::move(driver_host));
}

zx::status<fidl::ClientEnd<fio::Directory>> DriverRunner::CreateComponent(std::string name,
                                                                          std::string url,
                                                                          std::string collection) {
  auto endpoints = fidl::CreateEndpoints<fio::Directory>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }
  auto bind_callback = [name](fsys::Realm::BindChildResponse* response) {
    if (response->result.is_err()) {
      LOGF(ERROR, "Failed to bind component '%s': %u", name.data(), response->result.err());
    }
  };
  auto create_callback = [this, name, collection, server_end = std::move(endpoints->server),
                          bind_callback = std::move(bind_callback)](
                             fsys::Realm::CreateChildResponse* response) mutable {
    if (response->result.is_err()) {
      LOGF(ERROR, "Failed to create component '%s': %u", name.data(), response->result.err());
      return;
    }
    auto bind = realm_->BindChild(fsys::ChildRef{.name = fidl::unowned_str(name),
                                                 .collection = fidl::unowned_str(collection)},
                                  std::move(server_end), std::move(bind_callback));
    if (!bind.ok()) {
      LOGF(ERROR, "Failed to bind component '%s': %s", name.data(), bind.error());
    }
  };
  auto unowned_name = fidl::unowned_str(name);
  auto unowned_url = fidl::unowned_str(url);
  auto startup = fsys::StartupMode::LAZY;
  auto child_decl = fsys::ChildDecl::UnownedBuilder()
                        .set_name(fidl::unowned_ptr(&unowned_name))
                        .set_url(fidl::unowned_ptr(&unowned_url))
                        .set_startup(fidl::unowned_ptr(&startup));
  auto create = realm_->CreateChild(fsys::CollectionRef{.name = fidl::unowned_str(collection)},
                                    child_decl.build(), std::move(create_callback));
  if (!create.ok()) {
    LOGF(ERROR, "Failed to create component '%s': %s", name.data(), create.error());
    return zx::error(ZX_ERR_INTERNAL);
  }
  return zx::ok(std::move(endpoints->client));
}

uint64_t DriverRunner::NextId() { return next_id_++; }
