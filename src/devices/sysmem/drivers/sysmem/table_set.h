// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_TABLE_SET_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_TABLE_SET_H_

#include <lib/fidl/llcpp/fidl_allocator.h>

#include <unordered_set>

class TableHolderBase;

class TableSet {
 public:
  TableSet();

  fidl::AnyAllocator& allocator();

  void CountChurn();
  void MitigateChurn();

 private:
  friend class TableHolderBase;

  using FidlAllocator = fidl::FidlAllocator<>;

  void TrackTableHolder(TableHolderBase* table_holder);

  void UntrackTableHolder(TableHolderBase* table_holder);

  std::unique_ptr<FidlAllocator> allocator_;
  std::unordered_set<TableHolderBase*> tables_;
  uint32_t churn_count_ = 0;
};

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_TABLE_SET_H_
