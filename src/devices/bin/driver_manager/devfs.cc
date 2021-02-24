// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "devfs.h"

#include <fcntl.h>
#include <fuchsia/io/c/fidl.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/coding.h>
#include <lib/fidl/txn_header.h>
#include <lib/memfs/cpp/vnode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/device/vfs.h>
#include <zircon/fidl.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <algorithm>
#include <memory>

#include <ddk/driver.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/string.h>
#include <fbl/string_buffer.h>
#include <src/storage/deprecated-fs-fidl-handler/fidl-handler.h>

#include "async_loop_owned_rpc_handler.h"
#include "coordinator.h"
#include "lib/fidl/cpp/message_part.h"
#include "src/devices/lib/log/log.h"

namespace {

namespace fio = ::llcpp::fuchsia::io;

async_dispatcher_t* g_dispatcher = nullptr;
uint64_t next_ino = 2;

std::unique_ptr<Devnode> root_devnode;

std::unique_ptr<Devnode> class_devnode;

std::unique_ptr<Devnode> devfs_mkdir(Devnode* parent, const fbl::String& name);

// Dummy node to represent dev/diagnostics directory.
std::unique_ptr<Devnode> diagnostics_devnode;

// Connection to diagnostics VFS server. Channel is owned by inspect manager.
zx::unowned_channel diagnostics_channel;

const char kDiagnosticsDirName[] = "diagnostics";
const size_t kDiagnosticsDirLen = strlen(kDiagnosticsDirName);

zx::channel g_devfs_root;

}  // namespace

struct Watcher : fbl::DoublyLinkedListable<std::unique_ptr<Watcher>,
                                           fbl::NodeOptions::AllowRemoveFromContainer> {
  Watcher(Devnode* dn, zx::channel ch, uint32_t mask);

  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;

  Watcher(Watcher&&) = delete;
  Watcher& operator=(Watcher&&) = delete;

  void HandleChannelClose(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                          const zx_packet_signal_t* signal);

  Devnode* devnode = nullptr;
  zx::channel handle;
  uint32_t mask = 0;
  async::WaitMethod<Watcher, &Watcher::HandleChannelClose> channel_close_wait{this};
};

Watcher::Watcher(Devnode* dn, zx::channel ch, uint32_t mask)
    : devnode(dn), handle(std::move(ch)), mask(mask) {}

void Watcher::HandleChannelClose(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                 zx_status_t status, const zx_packet_signal_t* signal) {
  if (status == ZX_OK) {
    if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
      RemoveFromContainer();
    }
  }
}

class DevfsFidlServer;

class DcIostate : public fbl::DoublyLinkedListable<DcIostate*>,
                  public AsyncLoopOwnedRpcHandler<DcIostate> {
 public:
  explicit DcIostate(Devnode* dn);
  ~DcIostate();

  // Remove this DcIostate from its devnode
  void DetachFromDevnode();

  // Claims ownership of |*h| on success
  static zx_status_t Create(Devnode* dn, async_dispatcher_t* dispatcher, zx::channel* h);

  static zx_status_t DevfsFidlHandler(fidl_incoming_msg_t* msg, fidl_txn_t* txn, void* cookie,
                                      async_dispatcher_t* dispatcher);

  static void HandleRpc(std::unique_ptr<DcIostate> ios, async_dispatcher_t* dispatcher,
                        async::WaitBase* wait, zx_status_t status,
                        const zx_packet_signal_t* signal);

 private:
  friend class DevfsFidlServer;

  uint64_t readdir_ino_ = 0;

  // pointer to our devnode, nullptr if it has been removed
  Devnode* devnode_ = nullptr;

  std::unique_ptr<DevfsFidlServer> server_;
};


// This is a wrapper-adapter while the rest of the infrastructure here uses fidl_txn_t. It forwards
// fidl::Transaction::Reply() to fidl_txn_t.reply().
class TxnForwarder : public fidl::Transaction {
 public:
  explicit TxnForwarder(fidl_txn_t* txn) : txn_(txn) {}

  TxnForwarder& operator=(const TxnForwarder&) = delete;
  TxnForwarder(const TxnForwarder&) = delete;

  TxnForwarder& operator=(TxnForwarder&&) = delete;
  TxnForwarder(TxnForwarder&&) = delete;

  zx_status_t Reply(fidl::OutgoingMessage* message) override {
    if (closed_) {
      return status_;
    }
    return txn_->reply(txn_, message->message());
  }

  void Close(zx_status_t epitaph) override {
    closed_ = true;
    status_ = epitaph;
  }

  std::unique_ptr<Transaction> TakeOwnership() final {
    ZX_ASSERT_MSG(false, "TxnForwarder cannot take ownership.");
  }

  zx_status_t GetStatus() const {
    return status_;
  }

 private:
  fidl_txn_t* txn_;
  bool closed_ = false;
  zx_status_t status_ = ZX_OK;
};

// This is an llcpp-backed server equivalent to the switch() in DevfsFidlHandler(). Entries are
// DevfsFidlHandler() are being migrated to methods here. See https://fxbug.dev/68648.
class DevfsFidlServer : public fio::DirectoryAdmin::Interface {
 public:
  explicit DevfsFidlServer(DcIostate* iostate) : owner_(iostate) {}

  // Awful hacks for now to integrate with DcIostate::DevfsFidlHandler().
  void set_current_dispatcher(async_dispatcher_t* dispatcher) { current_dispatcher_ = dispatcher; }
  void clear_current_dispatcher() { current_dispatcher_ = nullptr; }

  void Clone(uint32_t flags, fidl::ServerEnd<fio::Node> object,
             CloneCompleter::Sync& completer) override;
  void Close(CloseCompleter::Sync& completer) override {}
  void Describe(DescribeCompleter::Sync& completer) override {}
  void Sync(SyncCompleter::Sync& completer) override {}
  void GetAttr(GetAttrCompleter::Sync& completer) override {}
  void SetAttr(uint32_t flags, fio::wire::NodeAttributes attributes,
               SetAttrCompleter::Sync& completer) override {}

  void Open(uint32_t flags, uint32_t mode, fidl::StringView path, fidl::ServerEnd<fio::Node> object,
            OpenCompleter::Sync& completer) override;
  void AddInotifyFilter(::llcpp::fuchsia::io2::wire::InotifyWatchMask filters,
                        fidl::StringView path, uint32_t watch_descriptor, zx::socket socket,
                        fidl::ServerEnd<::llcpp::fuchsia::io2::Inotifier> controller,
                        AddInotifyFilterCompleter::Sync& completer) override {}
  void Unlink(fidl::StringView path, UnlinkCompleter::Sync& completer) override {}
  void ReadDirents(uint64_t max_bytes, ReadDirentsCompleter::Sync& completer) override {}
  void Rewind(RewindCompleter::Sync& completer) override {}
  void GetToken(GetTokenCompleter::Sync& completer) override {}
  void Rename(fidl::StringView src, zx::handle dst_parent_token, fidl::StringView dst,
              RenameCompleter::Sync& completer) override {}
  void Link(fidl::StringView src, zx::handle dst_parent_token, fidl::StringView dst,
            LinkCompleter::Sync& completer) override {}
  void Watch(uint32_t mask, uint32_t options, zx::channel watcher,
             WatchCompleter::Sync& completer) override;
  void Mount(fidl::ClientEnd<fio::Directory> remote, MountCompleter::Sync& completer) override {}
  void MountAndCreate(fidl::ClientEnd<fio::Directory> remote, fidl::StringView name, uint32_t flags,
                      MountAndCreateCompleter::Sync& completer) override {}
  void Unmount(UnmountCompleter::Sync& completer) override {}
  void UnmountNode(UnmountNodeCompleter::Sync& completer) override {}
  void QueryFilesystem(QueryFilesystemCompleter::Sync& completer) override;
  void GetDevicePath(GetDevicePathCompleter::Sync& completer) override {}

 private:
  DcIostate* owner_;
  async_dispatcher_t* current_dispatcher_ = nullptr;
};

struct Devnode : public fbl::DoublyLinkedListable<Devnode*> {
  explicit Devnode(fbl::String name);
  ~Devnode();

  Devnode(const Devnode&) = delete;
  Devnode& operator=(const Devnode&) = delete;

  Devnode(Devnode&&) = delete;
  Devnode& operator=(Devnode&&) = delete;

  fbl::String name;
  uint64_t ino = 0;

  // nullptr if we are a pure directory node,
  // otherwise the device we are referencing
  Device* device = nullptr;

  fbl::DoublyLinkedList<std::unique_ptr<Watcher>> watchers;

  // list of our child devnodes
  fbl::DoublyLinkedList<Devnode*> children;

  // Pointer to our parent, for removing ourselves from its list of
  // children. Our parent must outlive us.
  Devnode* parent = nullptr;

  // list of attached iostates
  fbl::DoublyLinkedList<DcIostate*> iostate;

  // used to assign unique small device numbers
  // for class device links
  uint32_t seqcount = 0;
};

namespace {

struct ProtocolInfo {
  const char* name;
  std::unique_ptr<Devnode> devnode;
  uint32_t id;
  uint32_t flags;
};

ProtocolInfo proto_infos[] = {
#define DDK_PROTOCOL_DEF(tag, val, name, flags) {name, nullptr, val, flags},
#include <ddk/protodefs.h>
};

Devnode* proto_dir(uint32_t id) {
  for (const auto& info : proto_infos) {
    if (info.id == id) {
      return info.devnode.get();
    }
  }
  return nullptr;
}

void prepopulate_protocol_dirs() {
  class_devnode = devfs_mkdir(root_devnode.get(), "class");
  for (auto& info : proto_infos) {
    if (!(info.flags & PF_NOPUB)) {
      info.devnode = devfs_mkdir(class_devnode.get(), info.name);
    }
  }
}

void describe_error(zx::channel h, zx_status_t status) {
  fio::wire::NodeInfo invalid_node_info;
  fio::Node::OnOpenResponse::OwnedEncodedMessage response(status, invalid_node_info);
  response.Write(h.get());
}

// A devnode is a directory (from stat's perspective) if
// it has children, or if it doesn't have a device, or if
// its device has no rpc handle
bool devnode_is_dir(const Devnode* dn) {
  if (dn->children.is_empty()) {
    return (dn->device == nullptr) || (!dn->device->device_controller().channel().is_valid()) ||
           (!dn->device->channel()->is_valid());
  }
  return true;
}

// Local devnodes are ones that we should not hand off OPEN
// RPCs to the underlying driver_host
bool devnode_is_local(Devnode* dn) {
  if (dn->device == nullptr) {
    return true;
  }
  if (!dn->device->device_controller().channel().get()) {
    return true;
  }
  if (dn->device->flags & DEV_CTX_MUST_ISOLATE) {
    return true;
  }
  return false;
}

// Notify a single watcher about the given operation and path.  On failure,
// frees the watcher.  This can only be called on a watcher that has not yet
// been added to a Devnode's watchers list.
void devfs_notify_single(std::unique_ptr<Watcher>* watcher, const fbl::String& name, unsigned op) {
  size_t len = name.length();
  if (!*watcher || len > fio::MAX_FILENAME) {
    return;
  }

  ZX_ASSERT(!(*watcher)->InContainer());

  uint8_t msg[fio::MAX_FILENAME + 2];
  const uint32_t msg_len = static_cast<uint32_t>(len + 2);
  msg[0] = static_cast<uint8_t>(op);
  msg[1] = static_cast<uint8_t>(len);
  memcpy(msg + 2, name.c_str(), len);

  // convert to mask
  op = (1u << op);

  if (!((*watcher)->mask & op)) {
    return;
  }

  if ((*watcher)->handle.write(0, msg, msg_len, nullptr, 0) != ZX_OK) {
    watcher->reset();
  }
}

void devfs_notify(Devnode* dn, const fbl::String& name, unsigned op) {
  if (dn->watchers.is_empty()) {
    return;
  }

  size_t len = name.length();
  if (len > fio::MAX_FILENAME) {
    return;
  }

  uint8_t msg[fio::MAX_FILENAME + 2];
  const uint32_t msg_len = static_cast<uint32_t>(len + 2);
  msg[0] = static_cast<uint8_t>(op);
  msg[1] = static_cast<uint8_t>(len);
  memcpy(msg + 2, name.c_str(), len);

  // convert to mask
  op = (1u << op);

  for (auto itr = dn->watchers.begin(); itr != dn->watchers.end();) {
    auto& cur = *itr;
    // Advance the iterator now instead of at the end of the loop because we
    // may erase the current element from the list.
    ++itr;

    if (!(cur.mask & op)) {
      continue;
    }

    if (cur.handle.write(0, msg, msg_len, nullptr, 0) != ZX_OK) {
      dn->watchers.erase(cur);
      // The Watcher is free'd here
    }
  }
}

}  // namespace

zx_status_t devfs_watch(Devnode* dn, zx::channel h, uint32_t mask) {
  auto watcher = std::make_unique<Watcher>(dn, std::move(h), mask);
  if (watcher == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  // If the watcher has asked for all existing entries, send it all of them
  // followed by the end-of-existing marker (IDLE).
  if (mask & fio::WATCH_MASK_EXISTING) {
    for (const auto& child : dn->children) {
      if (child.device && (child.device->flags & DEV_CTX_INVISIBLE)) {
        continue;
      }
      // TODO: send multiple per write
      devfs_notify_single(&watcher, child.name, fio::WATCH_EVENT_EXISTING);
    }
    devfs_notify_single(&watcher, "", fio::WATCH_EVENT_IDLE);
  }

  // Watcher may have been freed by devfs_notify_single, so check before
  // adding.
  if (watcher) {
    dn->watchers.push_front(std::move(watcher));

    Watcher& watcher_ref = dn->watchers.front();
    watcher_ref.channel_close_wait.set_object(watcher_ref.handle.get());
    watcher_ref.channel_close_wait.set_trigger(ZX_CHANNEL_PEER_CLOSED);
    watcher_ref.channel_close_wait.Begin(g_dispatcher);
  }
  return ZX_OK;
}

bool devfs_has_watchers(Devnode* dn) { return !dn->watchers.is_empty(); }

namespace {

std::unique_ptr<Devnode> devfs_mknode(const fbl::RefPtr<Device>& dev, const fbl::String& name) {
  auto dn = std::make_unique<Devnode>(name);
  if (!dn) {
    return nullptr;
  }
  dn->ino = next_ino++;
  // TODO(teisenbe): This should probably be refcounted
  dn->device = dev.get();
  return dn;
}

std::unique_ptr<Devnode> devfs_mkdir(Devnode* parent, const fbl::String& name) {
  std::unique_ptr<Devnode> dn = devfs_mknode(nullptr, name);
  if (dn == nullptr) {
    return nullptr;
  }
  dn->parent = parent;
  parent->children.push_back(dn.get());
  return dn;
}

Devnode* devfs_lookup(Devnode* parent, const fbl::String& name) {
  for (auto& child : parent->children) {
    if (name == child.name) {
      return &child;
    }
  }
  return nullptr;
}

zx_status_t fill_dirent(vdirent_t* de, size_t delen, uint64_t ino, const fbl::String& name,
                        uint8_t type) {
  size_t len = name.length();
  size_t sz = sizeof(vdirent_t) + len;

  if (sz > delen || len > NAME_MAX) {
    return ZX_ERR_INVALID_ARGS;
  }
  de->ino = ino;
  de->size = static_cast<uint8_t>(len);
  de->type = type;
  memcpy(de->name, name.c_str(), len);
  return static_cast<zx_status_t>(sz);
}

zx_status_t devfs_readdir(Devnode* dn, uint64_t* ino_inout, void* data, size_t len) {
  char* ptr = static_cast<char*>(data);
  uint64_t ino = *ino_inout;

  for (const auto& child : dn->children) {
    if (child.ino <= ino) {
      continue;
    }
    if (child.device == nullptr) {
      // "pure" directories (like /dev/class/$NAME) do not show up
      // if they have no children, to avoid clutter and confusion.
      // They remain openable, so they can be watched.
      // An exception being /dev/diagnostics which is served by different VFS and
      // should be listed even though it has no DevNode childrens.
      if (child.children.is_empty() && child.ino != diagnostics_devnode->ino) {
        continue;
      }
    } else {
      // invisible devices also do not show up
      if (child.device->flags & DEV_CTX_INVISIBLE) {
        continue;
      }
    }
    ino = child.ino;
    auto vdirent = reinterpret_cast<vdirent_t*>(ptr);
    zx_status_t r = fill_dirent(vdirent, len, ino, child.name, VTYPE_TO_DTYPE(V_TYPE_DIR));
    if (r < 0) {
      break;
    }
    ptr += r;
    len -= r;
  }

  *ino_inout = ino;
  return static_cast<zx_status_t>(ptr - static_cast<char*>(data));
}

zx_status_t devfs_walk(Devnode** dn_inout, char* path) {
  Devnode* dn = *dn_inout;

again:
  if ((path == nullptr) || (path[0] == 0)) {
    *dn_inout = dn;
    return ZX_OK;
  }
  char* name = path;
  if ((path = strchr(path, '/')) != nullptr) {
    *path++ = 0;
  }
  if (name[0] == 0) {
    return ZX_ERR_BAD_PATH;
  }
  for (auto& child : dn->children) {
    if (!strcmp(child.name.c_str(), name)) {
      if (child.device && (child.device->flags & DEV_CTX_INVISIBLE)) {
        continue;
      }
      dn = &child;
      goto again;
    }
  }
  // The path only partially matched.
  return ZX_ERR_NOT_FOUND;
}

void devfs_open(Devnode* dirdn, async_dispatcher_t* dispatcher, zx_handle_t h, char* path,
                uint32_t flags) {
  zx::channel ipc(h);
  h = ZX_HANDLE_INVALID;

  // Filter requests for diagnostics path and pass it on to diagnostics vfs server.
  if (!strncmp(path, kDiagnosticsDirName, kDiagnosticsDirLen) &&
      (path[kDiagnosticsDirLen] == '\0' || path[kDiagnosticsDirLen] == '/')) {
    char* dir_path = path + kDiagnosticsDirLen;
    char current_dir[] = ".";
    if (dir_path[0] == '/') {
      dir_path++;
    } else {
      dir_path = current_dir;
    }
    fio::Directory::Call::Open(fidl::UnownedClientEnd<fio::Directory>(diagnostics_channel->get()),
                               flags, 0, fidl::unowned_str(dir_path, strlen(dir_path)),
                               fidl::ServerEnd<fio::Node>(std::move(ipc)));
    return;
  }

  if (!strcmp(path, ".")) {
    path = nullptr;
  }

  Devnode* dn = dirdn;
  zx_status_t r = devfs_walk(&dn, path);

  bool describe = flags & ZX_FS_FLAG_DESCRIBE;
  if (r != ZX_OK) {
    if (describe) {
      describe_error(std::move(ipc), r);
    }
    return;
  }

  // If we are a local-only node, or we are asked to not go remote, or we are asked to
  // open-as-a-directory, open locally:
  if (devnode_is_local(dn) || flags & (ZX_FS_FLAG_NOREMOTE | ZX_FS_FLAG_DIRECTORY)) {
    zx::unowned_channel unowned_ipc(ipc);
    if ((r = DcIostate::Create(dn, dispatcher, &ipc)) != ZX_OK) {
      if (describe) {
        describe_error(std::move(ipc), r);
      }
      return;
    }
    if (describe) {
      fio::wire::NodeInfo node_info;
      fidl::aligned<fio::DirectoryObject> directory;
      node_info.set_directory(fidl::unowned_ptr(&directory));
      fio::Node::OnOpenResponse::OwnedEncodedMessage response(ZX_OK, node_info);

      // Writing to unowned_ipc is safe because this is executing on the same
      // thread as the DcAsyncLoop(), so the handle can't be closed underneath us.
      response.Write(unowned_ipc->get());
    }
    return;
  }

  // Otherwise we will pass the request on to the remote.
  fio::Directory::Call::Open(
      fidl::UnownedClientEnd<fio::Directory>(dn->device->device_controller().channel().get()),
      flags, 0, ".", fidl::ServerEnd<fio::Node>(std::move(ipc)));
}

void devfs_remove(Devnode* dn) {
  if (dn->InContainer()) {
    dn->parent->children.erase(*dn);
  }

  // detach all connected iostates
  while (!dn->iostate.is_empty()) {
    dn->iostate.front().DetachFromDevnode();
  }

  // notify own file watcher
  if ((dn->device == nullptr) || !(dn->device->flags & DEV_CTX_INVISIBLE)) {
    devfs_notify(dn, "", fio::WATCH_EVENT_DELETED);
  }

  // disconnect from device and notify parent/link directory watchers
  if (dn->device != nullptr) {
    if (dn->device->self == dn) {
      dn->device->self = nullptr;

      if ((dn->device->parent() != nullptr) && (dn->device->parent()->self != nullptr) &&
          !(dn->device->flags & DEV_CTX_INVISIBLE)) {
        devfs_notify(dn->device->parent()->self, dn->name, fio::WATCH_EVENT_REMOVED);
      }
    }
    if (dn->device->link == dn) {
      dn->device->link = nullptr;

      if (!(dn->device->flags & DEV_CTX_INVISIBLE)) {
        Devnode* dir = proto_dir(dn->device->protocol_id());
        devfs_notify(dir, dn->name, fio::WATCH_EVENT_REMOVED);
      }
    }
    dn->device = nullptr;
  }

  // destroy all watchers
  dn->watchers.clear();

  // detach children
  // They will be unpublished when the devices they're associated with are
  // eventually destroyed.
  dn->children.clear();
}

}  // namespace

Devnode::Devnode(fbl::String name) : name(std::move(name)) {}

Devnode::~Devnode() { devfs_remove(this); }

DcIostate::DcIostate(Devnode* dn) : devnode_(dn), server_(std::make_unique<DevfsFidlServer>(this)) {
  devnode_->iostate.push_back(this);
}

DcIostate::~DcIostate() { DetachFromDevnode(); }

void DcIostate::DetachFromDevnode() {
  if (devnode_ != nullptr) {
    devnode_->iostate.erase(*this);
    devnode_ = nullptr;
  }
  set_channel(zx::channel());
}

zx_status_t DcIostate::Create(Devnode* dn, async_dispatcher_t* dispatcher, zx::channel* ipc) {
  auto ios = std::make_unique<DcIostate>(dn);
  if (ios == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  ios->set_channel(std::move(*ipc));
  zx_status_t status = DcIostate::BeginWait(&ios, dispatcher);
  if (status != ZX_OK) {
    // Take the handle back from |ios| so it doesn't close it when it's
    // destroyed
    *ipc = ios->set_channel(zx::channel());
  }
  return status;
}

void devfs_advertise(const fbl::RefPtr<Device>& dev) {
  if (dev->link) {
    Devnode* dir = proto_dir(dev->protocol_id());
    devfs_notify(dir, dev->link->name, fio::WATCH_EVENT_ADDED);
  }
  if (dev->self->parent) {
    devfs_notify(dev->self->parent, dev->self->name, fio::WATCH_EVENT_ADDED);
  }
}

// TODO: generate a MODIFIED event rather than back to back REMOVED and ADDED
void devfs_advertise_modified(const fbl::RefPtr<Device>& dev) {
  if (dev->link) {
    Devnode* dir = proto_dir(dev->protocol_id());
    devfs_notify(dir, dev->link->name, fio::WATCH_EVENT_REMOVED);
    devfs_notify(dir, dev->link->name, fio::WATCH_EVENT_ADDED);
  }
  if (dev->self->parent) {
    devfs_notify(dev->self->parent, dev->self->name, fio::WATCH_EVENT_REMOVED);
    devfs_notify(dev->self->parent, dev->self->name, fio::WATCH_EVENT_ADDED);
  }
}

zx_status_t devfs_publish(const fbl::RefPtr<Device>& parent, const fbl::RefPtr<Device>& dev) {
  if ((parent->self == nullptr) || (dev->self != nullptr) || (dev->link != nullptr)) {
    return ZX_ERR_INTERNAL;
  }

  std::unique_ptr<Devnode> dnself = devfs_mknode(dev, dev->name());
  if (dnself == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  if ((dev->protocol_id() == ZX_PROTOCOL_TEST_PARENT) ||
      (dev->protocol_id() == ZX_PROTOCOL_MISC_PARENT) || (dev->protocol_id() == ZX_PROTOCOL_MISC)) {
    // misc devices are singletons, not a class
    // in the sense of other device classes.
    // They do not get aliases in /dev/class/misc/...
    // instead they exist only under their parent
    // device.
    goto done;
  }

  // Create link in /dev/class/... if this id has a published class
  Devnode* dir;
  dir = proto_dir(dev->protocol_id());
  if (dir != nullptr) {
    char tmp[32];
    const char* name = dev->name().data();

    if (dev->protocol_id() != ZX_PROTOCOL_CONSOLE) {
      for (unsigned n = 0; n < 1000; n++) {
        snprintf(tmp, sizeof(tmp), "%03u", (dir->seqcount++) % 1000);
        if (devfs_lookup(dir, tmp) == nullptr) {
          name = tmp;
          goto got_name;
        }
      }
      return ZX_ERR_ALREADY_EXISTS;
    }

  got_name:
    std::unique_ptr<Devnode> dnlink = devfs_mknode(dev, name);
    if (dnlink == nullptr) {
      return ZX_ERR_NO_MEMORY;
    }

    // add link node to class directory
    dnlink->parent = dir;
    dir->children.push_back(dnlink.get());
    dev->link = dnlink.release();
  }

done:
  // add self node to parent directory
  dnself->parent = parent->self;
  parent->self->children.push_back(dnself.get());
  dev->self = dnself.release();

  if (!(dev->flags & DEV_CTX_INVISIBLE)) {
    devfs_advertise(dev);
  }
  return ZX_OK;
}

// TODO(teisenbe): Ideally this would take a RefPtr, but currently this is
// invoked in the dtor for Device.
void devfs_unpublish(Device* dev) {
  if (dev->self != nullptr) {
    delete dev->self;
    dev->self = nullptr;
  }
  if (dev->link != nullptr) {
    delete dev->link;
    dev->link = nullptr;
  }
}

zx_status_t devfs_connect(const Device* dev, zx::channel client_remote) {
  if (!client_remote.is_valid()) {
    return ZX_ERR_BAD_HANDLE;
  }
  fio::Directory::Call::Open(
      fidl::UnownedClientEnd<fio::Directory>(dev->device_controller().channel().get()), 0, 0, ".",
      fidl::ServerEnd<fio::Node>(std::move(client_remote)));
  return ZX_OK;
}

void devfs_connect_diagnostics(zx::unowned_channel h) { diagnostics_channel = h; }

// Helper macros for |DevfsFidlHandler| which make it easier
// avoid typing generated names.

// Decode the incoming request, returning an error and consuming
// all handles on error.
#define DECODE_REQUEST(MSG, METHOD)                                                         \
  do {                                                                                      \
    zx_status_t r;                                                                          \
    if ((r = fidl_decode_msg(&fuchsia_io_##METHOD##RequestTable, msg, nullptr)) != ZX_OK) { \
      return r;                                                                             \
    }                                                                                       \
  } while (0);

// Define a variable |request| from the incoming method, of
// the requested type.
#define DEFINE_REQUEST(MSG, METHOD) \
  fuchsia_io_##METHOD##Request* request = (fuchsia_io_##METHOD##Request*)MSG->bytes;

zx_status_t DcIostate::DevfsFidlHandler(fidl_incoming_msg_t* msg, fidl_txn_t* txn, void* cookie,
                                        async_dispatcher_t* dispatcher) {
  auto ios = static_cast<DcIostate*>(cookie);
  Devnode* dn = ios->devnode_;
  if (dn == nullptr) {
    return ZX_ERR_PEER_CLOSED;
  }

  TxnForwarder transaction(txn);

  auto hdr = static_cast<fidl_message_header_t*>(msg->bytes);

  zx_status_t r;
  uint64_t ordinal = hdr->ordinal;
  switch (ordinal) {
    case fuchsia_io_NodeCloneOrdinal: {
      ios->server_->set_current_dispatcher(dispatcher);
      auto result = fio::DirectoryAdmin::Dispatch(ios->server_.get(), msg, &transaction);
      ios->server_->clear_current_dispatcher();
      ZX_ASSERT(result == fidl::DispatchResult::kFound);
      return transaction.GetStatus();
    }
    case fuchsia_io_NodeDescribeOrdinal: {
      DECODE_REQUEST(msg, NodeDescribe);

      fio::wire::NodeInfo node_info;
      fidl::aligned<fio::DirectoryObject> directory;
      node_info.set_directory(fidl::unowned_ptr(&directory));
      fio::Node::DescribeResponse::OwnedEncodedMessage response(node_info);
      const auto& out = response.GetOutgoingMessage();

      fidl_outgoing_msg_t raw_msg = {
          .type = FIDL_OUTGOING_MSG_TYPE_BYTE,
          .byte =
              {
                  .bytes = out.bytes(),
                  .handles = out.handles(),
                  .num_bytes = out.byte_actual(),
                  .num_handles = out.handle_actual(),
              },
      };
      return txn->reply(txn, &raw_msg);
    }
    case fuchsia_io_DirectoryOpenOrdinal: {
      ios->server_->set_current_dispatcher(dispatcher);
      auto result = fio::DirectoryAdmin::Dispatch(ios->server_.get(), msg, &transaction);
      ios->server_->clear_current_dispatcher();
      ZX_ASSERT(result == fidl::DispatchResult::kFound);
      return transaction.GetStatus();
    }
    case fuchsia_io_NodeGetAttrOrdinal: {
      DECODE_REQUEST(msg, NodeGetAttr);
      uint32_t mode;
      if (devnode_is_dir(dn)) {
        mode = V_TYPE_DIR | V_IRUSR | V_IWUSR;
      } else {
        mode = V_TYPE_CDEV | V_IRUSR | V_IWUSR;
      }

      fuchsia_io_NodeAttributes attributes;
      memset(&attributes, 0, sizeof(attributes));
      attributes.mode = mode;
      attributes.content_size = 0;
      attributes.link_count = 1;
      attributes.id = dn->ino;
      return fuchsia_io_NodeGetAttr_reply(txn, ZX_OK, &attributes);
    }
    case fuchsia_io_DirectoryRewindOrdinal: {
      DECODE_REQUEST(msg, DirectoryRewind);
      ios->readdir_ino_ = 0;
      return fuchsia_io_DirectoryRewind_reply(txn, ZX_OK);
    }
    case fuchsia_io_DirectoryReadDirentsOrdinal: {
      DECODE_REQUEST(msg, DirectoryReadDirents);
      DEFINE_REQUEST(msg, DirectoryReadDirents);

      if (request->max_bytes > fio::MAX_BUF) {
        return fuchsia_io_DirectoryReadDirents_reply(txn, ZX_ERR_INVALID_ARGS, nullptr, 0);
      }

      uint8_t data[fio::MAX_BUF];
      size_t actual = 0;
      r = devfs_readdir(dn, &ios->readdir_ino_, data, request->max_bytes);
      if (r >= 0) {
        actual = r;
        r = ZX_OK;
      }
      return fuchsia_io_DirectoryReadDirents_reply(txn, r, data, actual);
    }
    case fuchsia_io_DirectoryWatchOrdinal: {
      ios->server_->set_current_dispatcher(dispatcher);
      auto result = fio::DirectoryAdmin::Dispatch(ios->server_.get(), msg, &transaction);
      ios->server_->clear_current_dispatcher();
      ZX_ASSERT(result == fidl::DispatchResult::kFound);
      return transaction.GetStatus();
    }
    case fuchsia_io_DirectoryAdminQueryFilesystemOrdinal: {
      ios->server_->set_current_dispatcher(dispatcher);
      auto result = fio::DirectoryAdmin::Dispatch(ios->server_.get(), msg, &transaction);
      ios->server_->clear_current_dispatcher();
      ZX_ASSERT(result == fidl::DispatchResult::kFound);
      return transaction.GetStatus();
    }
  }  // switch

  // close inbound handles so they do not leak
  FidlHandleInfoCloseMany(msg->handles, msg->num_handles);
  return ZX_ERR_NOT_SUPPORTED;
}

void DevfsFidlServer::Open(uint32_t flags, uint32_t mode, fidl::StringView path,
                           fidl::ServerEnd<fio::Node> object, OpenCompleter::Sync& completer) {
  if (path.size() <= fio::MAX_PATH) {
    fbl::StringBuffer<fio::MAX_PATH + 1> terminated_path;
    terminated_path.Append(path.data(), path.size());
    devfs_open(owner_->devnode_, current_dispatcher_, object.TakeChannel().release(),
               terminated_path.data(), flags);
  }
}

void DevfsFidlServer::Clone(uint32_t flags, fidl::ServerEnd<fio::Node> object,
                            CloneCompleter::Sync& completer) {
  if (flags & ZX_FS_FLAG_CLONE_SAME_RIGHTS) {
    flags |= ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE;
  }
  char path[] = ".";
  devfs_open(owner_->devnode_, current_dispatcher_, object.TakeChannel().release(), path,
             flags | ZX_FS_FLAG_NOREMOTE);
}

void DevfsFidlServer::QueryFilesystem(QueryFilesystemCompleter::Sync& completer) {
  fidl::tracking_ptr<fio::FilesystemInfo> info(std::make_unique<fio::FilesystemInfo>());
  strlcpy(reinterpret_cast<char*>(info->name.data()), "devfs", fio::MAX_FS_NAME_BUFFER);
  completer.Reply(ZX_OK, std::move(info));
}

void DevfsFidlServer::Watch(uint32_t mask, uint32_t options, zx::channel watcher,
                            WatchCompleter::Sync& completer) {
  zx_status_t status;
  if (mask & (~fio::WATCH_MASK_ALL) || options != 0) {
    status = ZX_ERR_INVALID_ARGS;
  } else {
    status = devfs_watch(owner_->devnode_, std::move(watcher), mask);
  }
  completer.Reply(status);
}

void DcIostate::HandleRpc(std::unique_ptr<DcIostate> ios, async_dispatcher_t* dispatcher,
                          async::WaitBase* wait, zx_status_t status,
                          const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to wait for RPC: %s", zx_status_get_string(status));
    return;
  }

  if (signal->observed & ZX_CHANNEL_READABLE) {
    status = fs::ReadMessage(
        wait->object(), [&ios, dispatcher](fidl_incoming_msg_t* msg, fs::FidlConnection* txn) {
          return DcIostate::DevfsFidlHandler(msg, txn->Txn(), ios.get(), dispatcher);
        });
    if (status == ZX_OK) {
      ios->BeginWait(std::move(ios), dispatcher);
      return;
    }
  } else if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
    fs::CloseMessage([&ios, dispatcher](fidl_incoming_msg_t* msg, fs::FidlConnection* txn) {
      return DcIostate::DevfsFidlHandler(msg, txn->Txn(), ios.get(), dispatcher);
    });
  } else {
    LOGF(FATAL, "Unexpected signal state %#08x", signal->observed);
  }
  // Do not start waiting again, and destroy |ios|
}

zx::unowned_channel devfs_root_borrow() { return zx::unowned_channel(g_devfs_root); }

zx::channel devfs_root_clone() { return zx::channel(fdio_service_clone(g_devfs_root.get())); }

void devfs_init(const fbl::RefPtr<Device>& device, async_dispatcher_t* dispatcher) {
  g_dispatcher = dispatcher;
  root_devnode = std::make_unique<Devnode>("");
  if (!root_devnode) {
    return;
  }
  root_devnode->ino = 1;

  prepopulate_protocol_dirs();

  // Create dummy diagnostics devnode, so that the directory is listed.
  diagnostics_devnode = devfs_mkdir(root_devnode.get(), "diagnostics");

  // TODO(teisenbe): Should this take a reference?
  root_devnode->device = device.get();
  root_devnode->device->self = root_devnode.get();

  zx::channel h0, h1;
  if (zx::channel::create(0, &h0, &h1) != ZX_OK) {
    return;
  } else if (DcIostate::Create(root_devnode.get(), dispatcher, &h0) != ZX_OK) {
    return;
  }

  g_devfs_root = std::move(h1);
  // This is actually owned by |device| and will be freed in unpublish
  __UNUSED auto ptr = root_devnode.release();
}

zx_status_t devfs_walk(Devnode* dn, const char* path, fbl::RefPtr<Device>* dev) {
  Devnode* inout = dn;

  char path_copy[PATH_MAX];
  if (strlen(path) + 1 > sizeof(path_copy)) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  }
  strcpy(path_copy, path);

  zx_status_t status = devfs_walk(&inout, path_copy);
  if (status != ZX_OK) {
    return status;
  }
  *dev = fbl::RefPtr(inout->device);
  return ZX_OK;
}
