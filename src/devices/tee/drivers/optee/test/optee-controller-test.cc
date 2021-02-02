// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "../optee-controller.h"

#include <fuchsia/hardware/platform/device/cpp/banjo.h>
#include <fuchsia/hardware/sysmem/cpp/banjo.h>
#include <lib/fake-bti/bti.h>
#include <lib/fake-object/object.h>
#include <lib/fake-resource/resource.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/zx/bti.h>
#include <stdlib.h>

#include <ddktl/suspend-txn.h>
#include <zxtest/zxtest.h>

#include "../optee-smc.h"
#include "../tee-smc.h"

struct SharedMemoryInfo {
  zx_paddr_t address = 0;
  size_t size = 0;
};

// This will be populated once the FakePdev creates the fake contiguous vmo so we can use the
// physical addresses within it.
static SharedMemoryInfo gSharedMemory = {};

zx_status_t zx_smc_call(zx_handle_t handle, const zx_smc_parameters_t* parameters,
                        zx_smc_result_t* out_smc_result) {
  EXPECT_TRUE(parameters);
  EXPECT_TRUE(out_smc_result);
  switch (parameters->func_id) {
    case tee_smc::kTrustedOsCallUidFuncId:
      out_smc_result->arg0 = optee::kOpteeApiUid_0;
      out_smc_result->arg1 = optee::kOpteeApiUid_1;
      out_smc_result->arg2 = optee::kOpteeApiUid_2;
      out_smc_result->arg3 = optee::kOpteeApiUid_3;
      break;
    case tee_smc::kTrustedOsCallRevisionFuncId:
      out_smc_result->arg0 = optee::kOpteeApiRevisionMajor;
      out_smc_result->arg1 = optee::kOpteeApiRevisionMinor;
      break;
    case optee::kGetOsRevisionFuncId:
      out_smc_result->arg0 = 1;
      out_smc_result->arg1 = 0;
      break;
    case optee::kExchangeCapabilitiesFuncId:
      out_smc_result->arg0 = optee::kReturnOk;
      out_smc_result->arg1 =
          optee::kSecureCapHasReservedSharedMem | optee::kSecureCapCanUsePrevUnregisteredSharedMem;
      break;
    case optee::kGetSharedMemConfigFuncId:
      out_smc_result->arg0 = optee::kReturnOk;
      out_smc_result->arg1 = gSharedMemory.address;
      out_smc_result->arg2 = gSharedMemory.size;
      break;
    default:
      return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_OK;
}

namespace optee {
namespace {

class FakePDev : public ddk::PDevProtocol<FakePDev, ddk::base_protocol> {
 public:
  FakePDev() : proto_({&pdev_protocol_ops_, this}) {}

  const pdev_protocol_t* proto() const { return &proto_; }

  zx_status_t PDevGetMmio(uint32_t index, pdev_mmio_t* out_mmio) {
    EXPECT_EQ(index, 0);
    constexpr size_t kSecureWorldMemorySize = 0x20000;

    EXPECT_OK(zx::vmo::create_contiguous(*fake_bti_, 0x20000, 0, &fake_vmo_));

    // Briefly pin the vmo to get the paddr for populating the gSharedMemory object
    zx_paddr_t secure_world_paddr;
    zx::pmt pmt;
    EXPECT_OK(fake_bti_->pin(ZX_BTI_PERM_READ | ZX_BTI_CONTIGUOUS, fake_vmo_, 0,
                             kSecureWorldMemorySize, &secure_world_paddr, 1, &pmt));
    // Use the second half of the secure world range to use as shared memory
    gSharedMemory.address = secure_world_paddr + (kSecureWorldMemorySize / 2);
    gSharedMemory.size = kSecureWorldMemorySize / 2;
    EXPECT_OK(pmt.unpin());

    out_mmio->vmo = fake_vmo_.get();
    out_mmio->offset = 0;
    out_mmio->size = kSecureWorldMemorySize;
    return ZX_OK;
  }

  zx_status_t PDevGetBti(uint32_t index, zx::bti* out_bti) {
    zx_status_t status = fake_bti_create(out_bti->reset_and_get_address());
    // Stash an unowned copy of it, for the purposes of creating a contiguous vmo to back the secure
    // world memory
    fake_bti_ = out_bti->borrow();
    return status;
  }

  zx_status_t PDevGetSmc(uint32_t index, zx::resource* out_resource) {
    // Just use a fake root resource for now, which is technically eligible for SMC calls. A more
    // appropriate object would be to use the root resource to mint an SMC resource type.
    return fake_root_resource_create(out_resource->reset_and_get_address());
  }

  zx_status_t PDevGetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out_irq) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t PDevGetDeviceInfo(pdev_device_info_t* out_info) { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t PDevGetBoardInfo(pdev_board_info_t* out_info) { return ZX_ERR_NOT_SUPPORTED; }

 private:
  pdev_protocol_t proto_;
  zx::unowned_bti fake_bti_;
  zx::vmo fake_vmo_;
};

class FakeSysmem : public ddk::SysmemProtocol<FakeSysmem> {
 public:
  FakeSysmem() : proto_({&sysmem_protocol_ops_, this}) {}

  const sysmem_protocol_t* proto() const { return &proto_; }

  zx_status_t SysmemConnect(zx::channel allocator2_request) { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t SysmemRegisterHeap(uint64_t heap, zx::channel heap_connection) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t SysmemRegisterSecureMem(zx::channel tee_connection) { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t SysmemUnregisterSecureMem() { return ZX_ERR_NOT_SUPPORTED; }

 private:
  sysmem_protocol_t proto_;
};

class FakeRpmb : public ddk::RpmbProtocol<FakeRpmb> {
 public:
  FakeRpmb() : proto_({&rpmb_protocol_ops_, this}) {}

  const rpmb_protocol_t* proto() const { return &proto_; }
  void RpmbConnectServer(zx::channel server) { call_cnt++; }

  int get_call_count() const { return call_cnt; }

  void reset() { call_cnt = 0; }

 private:
  rpmb_protocol_t proto_;
  int call_cnt = 0;
};

class FakeDdkOptee : public zxtest::Test {
 public:
  void SetUp() override {
    fbl::Array<fake_ddk::FragmentEntry> fragments(new fake_ddk::FragmentEntry[3], 3);
    fragments[0].name = "fuchsia.hardware.platform.device.PDev";
    fragments[0].protocols.emplace_back(fake_ddk::ProtocolEntry{
        ZX_PROTOCOL_PDEV, *reinterpret_cast<const fake_ddk::Protocol*>(pdev_.proto())});
    fragments[1].name = "sysmem";
    fragments[1].protocols.emplace_back(fake_ddk::ProtocolEntry{
        ZX_PROTOCOL_SYSMEM, *reinterpret_cast<const fake_ddk::Protocol*>(sysmem_.proto())});
    fragments[2].name = "rpmb";
    fragments[2].protocols.emplace_back(fake_ddk::ProtocolEntry{
        ZX_PROTOCOL_RPMB, *reinterpret_cast<const fake_ddk::Protocol*>(rpmb_.proto())});
    ddk_.SetFragments(std::move(fragments));
  }

  void TearDown() override {
    optee_.DdkAsyncRemove();
    EXPECT_TRUE(ddk_.Ok());
  }

 protected:
  OpteeController optee_{fake_ddk::kFakeParent};

  FakePDev pdev_;
  FakeSysmem sysmem_;
  FakeRpmb rpmb_;

  // Fake ddk must be destroyed before optee because it may be executing messages against optee on
  // another thread.
  fake_ddk::Bind ddk_;
};

TEST_F(FakeDdkOptee, PmtUnpinned) {
  EXPECT_EQ(optee_.Bind(), ZX_OK);

  zx_handle_t pmt_handle = optee_.pmt().get();
  EXPECT_NE(pmt_handle, ZX_HANDLE_INVALID);

  EXPECT_TRUE(fake_object::FakeHandleTable().Get(pmt_handle).is_ok());
  EXPECT_EQ(fake_object::HandleType::PMT, fake_object::FakeHandleTable().Get(pmt_handle)->type());

  optee_.DdkSuspend(ddk::SuspendTxn{fake_ddk::kFakeDevice, DEV_POWER_STATE_D3COLD, false,
                                    DEVICE_SUSPEND_REASON_REBOOT});
  EXPECT_FALSE(fake_object::FakeHandleTable().Get(pmt_handle).is_ok());
}

TEST_F(FakeDdkOptee, RpmbTest) {
  EXPECT_EQ(optee_.Bind(), ZX_OK);

  rpmb_.reset();

  zx::channel client, server;
  EXPECT_EQ(optee_.RpmbConnectServer(std::move(server)), ZX_ERR_INVALID_ARGS);
  EXPECT_EQ(rpmb_.get_call_count(), 0);

  EXPECT_EQ(zx::channel::create(0, &client, &server), ZX_OK);
  EXPECT_EQ(optee_.RpmbConnectServer(std::move(server)), ZX_OK);
  EXPECT_EQ(rpmb_.get_call_count(), 1);
}

}  // namespace
}  // namespace optee
