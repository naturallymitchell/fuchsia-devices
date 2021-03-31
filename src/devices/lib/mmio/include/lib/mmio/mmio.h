// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_MMIO_INCLUDE_LIB_MMIO_MMIO_H_
#define SRC_DEVICES_LIB_MMIO_INCLUDE_LIB_MMIO_MMIO_H_

#include <lib/ddk/debug.h>
#include <lib/ddk/hw/arch_ops.h>
#include <lib/ddk/mmio-buffer.h>
#include <lib/mmio-ptr/mmio-ptr.h>
#include <lib/zx/bti.h>
#include <lib/zx/resource.h>
#include <lib/zx/vmo.h>
#include <string.h>
#include <zircon/assert.h>
#include <zircon/process.h>

#include <optional>
#include <utility>

namespace ddk {

// Usage Notes:
//
// ddk::MmioBuffer is a c++ wrapper around the mmio_buffer_t object. It provides
// capabilities to map an MMIO region provided by a VMO, and accessors to read and
// write the MMIO region. Destroying it will result in the MMIO region being
// unmapped. All read/write operations are bounds checked.
//
// ddk::MmioView provides a slice view of an mmio region. It provides the same
// accessors provided by MmioBuffer, but does not manage the buffer's mapping.
// It must not outlive the ddk::MmioBuffer it is spawned from.
//
// ddk::MmioPinnedBuffer is a c++ wrapper around the mmio_pinned_buffer_t object.
// It is generated by calling |Pin()| on a MmioBuffer or MmioView and provides
// access to the physical address space for the region. Performing pinning on MmioView
// will only pin the pages associated with the MmioView, and not the entire
// MmioBuffer. Destorying MmioPInnedBuffer will unpin the memory.
//
// Consider using this in conjuntion with hwreg::RegisterBase for increased safety
// and improved ergonomics.
//
////////////////////////////////////////////////////////////////////////////////
// Example: An mmio region provided by the Platform Device protocol.
//
// pdev_mmio_t pdev_mmio;
// GetMmio(index, &pdev_mmio);
//
// std::optional<ddk::MmioBuffer> mmio;
// zx_status_t status;
//
// status = ddk::MmioBuffer::Create(pdev_mmio.offset, pdev_mmio.size,
//                                  zx::vmo(pdev_mmio.vmo),
//                                  ZX_CACHE_POLICY_UNCACHED_DEVICE, mmio);
// if (status != ZX_OK) return status;
//
// auto value = mmio->Read<uint32_t>(kRegOffset);
// value |= kRegFlag;
// mmio->Write(value, kRegOffset);
//
////////////////////////////////////////////////////////////////////////////////
// Example: An mmio region created from a physical region.
//
// std::optional<ddk::MmioBuffer> mmio;
// zx_status_t status;
//
// Please do not use get_root_resource() in new code. See fxbug.dev/31358.
// zx::unowned_resource resource(get_root_resource());
// status = ddk::MmioBuffer::Create(T931_USBPHY21_BASE, T931_USBPHY21_LENGTH,
//                                  *resource, ZX_CACHE_POLICY_UNCACHED_DEVICE,
//                                  &mmio);
// if (status != ZX_OK) return status;
//
// mmio->SetBits(kRegFlag, kRegOffset);
//
////////////////////////////////////////////////////////////////////////////////
// Example: An mmio region which is pinned in order to perform dma. Using views
// to increase safetey.
//
// std::optional<ddk::MmioBuffer> mmio;
// status = ddk::MmioBuffer::Create(pdev_mmio.offset, pdev_mmio.size,
//                                  zx::vmo(pdev_mmio.vmo),
//                                  ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
// if (status != ZX_OK) return status;
//
// ddk::MmioView dma_region = mmio->View(kDmaRegionOffset, kDmaRegionSize);
// ddk::MmioView dma_ctrl = mmio->View(kDmaCtrlRegOffset, kDmaCtrlRegSize);
//
// std::optional<ddk::MmioPinnedBuffer> dma_pinned_region;
// status = dma_region->Pin(&bti_, &dma_pinned_region);
// if (status != ZX_OK) return status;
//
// dma_ctrl->Write<uint64_t>(dma_pinnedRegion->get_paddr(), kPaddrOffset);
// <...>
//

// MmioPinnedBuffer is wrapper around mmio_pinned_buffer_t.
class MmioPinnedBuffer {
 public:
  // DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE
  MmioPinnedBuffer(const MmioPinnedBuffer&) = delete;
  MmioPinnedBuffer& operator=(const MmioPinnedBuffer&) = delete;

  explicit MmioPinnedBuffer(mmio_pinned_buffer_t pinned) : pinned_(pinned) {
    ZX_ASSERT(pinned_.paddr != 0);
  }

  ~MmioPinnedBuffer() { mmio_buffer_unpin(&pinned_); }

  MmioPinnedBuffer(MmioPinnedBuffer&& other) { transfer(std::move(other)); }

  MmioPinnedBuffer& operator=(MmioPinnedBuffer&& other) {
    transfer(std::move(other));
    return *this;
  }

  void reset() {
    mmio_buffer_unpin(&pinned_);
    memset(&pinned_, 0, sizeof(pinned_));
  }

  zx_paddr_t get_paddr() const { return pinned_.paddr; }

 private:
  void transfer(MmioPinnedBuffer&& other) {
    pinned_ = other.pinned_;
    memset(&other.pinned_, 0, sizeof(other.pinned_));
  }
  mmio_pinned_buffer_t pinned_;
};

struct MmioBufferOps {
  uint8_t (*Read8)(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs);
  uint16_t (*Read16)(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs);
  uint32_t (*Read32)(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs);
  uint64_t (*Read64)(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs);
  void (*ReadBuffer)(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs, void* buffer,
                     size_t size);

  void (*Write8)(const void* ctx, const mmio_buffer_t& mmio, uint8_t val, zx_off_t offs);
  void (*Write16)(const void* ctx, const mmio_buffer_t& mmio, uint16_t val, zx_off_t offs);
  void (*Write32)(const void* ctx, const mmio_buffer_t& mmio, uint32_t val, zx_off_t offs);
  void (*Write64)(const void* ctx, const mmio_buffer_t& mmio, uint64_t val, zx_off_t offs);
  void (*WriteBuffer)(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs, const void* buffer,
                      size_t size);
};

// Forward declaration.
class MmioView;

// MmioBuffer is wrapper around mmio_block_t.
class MmioBuffer {
 public:
  // DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE
  MmioBuffer(const MmioBuffer&) = delete;
  MmioBuffer& operator=(const MmioBuffer&) = delete;

  MmioBuffer(mmio_buffer_t mmio, const MmioBufferOps* ops = &kDefaultOps, const void* ctx = nullptr)
      : mmio_(mmio), ops_(ops), ctx_(ctx) {
    ZX_ASSERT(mmio_.vaddr != nullptr);
  }

  virtual ~MmioBuffer() { mmio_buffer_release(&mmio_); }

  MmioBuffer(MmioBuffer&& other) { transfer(std::move(other)); }

  MmioBuffer& operator=(MmioBuffer&& other) {
    transfer(std::move(other));
    return *this;
  }

  static zx_status_t Create(zx_off_t offset, size_t size, zx::vmo vmo, uint32_t cache_policy,
                            std::optional<MmioBuffer>* mmio_buffer) {
    mmio_buffer_t mmio;
    zx_status_t status = mmio_buffer_init(&mmio, offset, size, vmo.release(), cache_policy);
    if (status == ZX_OK) {
      *mmio_buffer = MmioBuffer(mmio);
    }
    return status;
  }

  static zx_status_t Create(zx_paddr_t base, size_t size, const zx::resource& resource,
                            uint32_t cache_policy, std::optional<MmioBuffer>* mmio_buffer) {
    mmio_buffer_t mmio;
    zx_status_t status = mmio_buffer_init_physical(&mmio, base, size, resource.get(), cache_policy);
    if (status == ZX_OK) {
      *mmio_buffer = MmioBuffer(mmio);
    }
    return status;
  }

  void reset() {
    mmio_buffer_release(&mmio_);
    memset(&mmio_, 0, sizeof(mmio_));
  }

  void Info() const {
    zxlogf(INFO, "vaddr = %p", get());
    zxlogf(INFO, "size = %lu", get_size());
  }

  MMIO_PTR void* get() const { return mmio_.vaddr; }
  zx_off_t get_offset() const { return mmio_.offset; }
  size_t get_size() const { return mmio_.size; }
  zx::unowned_vmo get_vmo() const { return zx::unowned_vmo(mmio_.vmo); }

  zx_status_t Pin(const zx::bti& bti, std::optional<MmioPinnedBuffer>* pinned_buffer) {
    mmio_pinned_buffer_t pinned;
    zx_status_t status = mmio_buffer_pin(&mmio_, bti.get(), &pinned);
    if (status == ZX_OK) {
      *pinned_buffer = MmioPinnedBuffer(pinned);
    }
    return status;
  }

  // Provides a slice view into the mmio.
  // The returned slice object must not outlive this object.
  MmioView View(zx_off_t off) const;
  MmioView View(zx_off_t off, size_t size) const;

  uint32_t ReadMasked32(uint32_t mask, zx_off_t offs) const {
    return ReadMasked<uint32_t>(mask, offs);
  }

  void ModifyBits32(uint32_t bits, uint32_t mask, zx_off_t offs) const {
    ModifyBits<uint32_t>(bits, mask, offs);
  }

  void ModifyBits32(uint32_t val, uint32_t start, uint32_t width, zx_off_t offs) const {
    ModifyBits<uint32_t>(val, start, width, offs);
  }

  void SetBits32(uint32_t bits, zx_off_t offs) const { SetBits<uint32_t>(bits, offs); }

  void ClearBits32(uint32_t bits, zx_off_t offs) const { ClearBits<uint32_t>(bits, offs); }

  void CopyFrom32(const MmioBuffer& source, zx_off_t source_offs, zx_off_t dest_offs,
                  size_t count) const {
    CopyFrom<uint32_t>(source, source_offs, dest_offs, count);
  }

  template <typename T>
  T Read(zx_off_t offs) const {
    if constexpr (sizeof(T) == sizeof(uint8_t)) {
      return Read8(offs);
    } else if constexpr (sizeof(T) == sizeof(uint16_t)) {
      return Read16(offs);
    } else if constexpr (sizeof(T) == sizeof(uint32_t)) {
      return Read32(offs);
    } else if constexpr (sizeof(T) == sizeof(uint64_t)) {
      return Read64(offs);
    } else {
      static_assert(always_false<T>);
    }
  }

  template <typename T>
  T ReadMasked(T mask, zx_off_t offs) const {
    return (Read<T>(offs) & mask);
  }

  template <typename T>
  void CopyFrom(const MmioBuffer& source, zx_off_t source_offs, zx_off_t dest_offs,
                size_t count) const {
    for (size_t i = 0; i < count; i++) {
      T val = source.Read<T>(source_offs);
      Write<T>(val, dest_offs);
      source_offs = source_offs + sizeof(T);
      dest_offs = dest_offs + sizeof(T);
    }
  }

  template <typename T>
  void Write(T val, zx_off_t offs) const {
    if constexpr (sizeof(T) == sizeof(uint8_t)) {
      Write8(val, offs);
    } else if constexpr (sizeof(T) == sizeof(uint16_t)) {
      Write16(val, offs);
    } else if constexpr (sizeof(T) == sizeof(uint32_t)) {
      Write32(val, offs);
    } else if constexpr (sizeof(T) == sizeof(uint64_t)) {
      Write64(val, offs);
    } else {
      static_assert(always_false<T>);
    }
  }

  template <typename T>
  void ModifyBits(T bits, T mask, zx_off_t offs) const {
    T val = Read<T>(offs);
    Write<T>(static_cast<T>((val & ~mask) | (bits & mask)), offs);
  }

  template <typename T>
  void SetBits(T bits, zx_off_t offs) const {
    ModifyBits<T>(bits, bits, offs);
  }

  template <typename T>
  void ClearBits(T bits, zx_off_t offs) const {
    ModifyBits<T>(0, bits, offs);
  }

  template <typename T>
  T GetBits(size_t shift, size_t count, zx_off_t offs) const {
    T mask = static_cast<T>(((static_cast<T>(1) << count) - 1) << shift);
    T val = Read<T>(offs);
    return static_cast<T>((val & mask) >> shift);
  }

  template <typename T>
  T GetBit(size_t shift, zx_off_t offs) const {
    return GetBits<T>(shift, 1, offs);
  }

  template <typename T>
  void ModifyBits(T bits, size_t shift, size_t count, zx_off_t offs) const {
    T mask = static_cast<T>(((static_cast<T>(1) << count) - 1) << shift);
    T val = Read<T>(offs);
    Write<T>(static_cast<T>((val & ~mask) | ((bits << shift) & mask)), offs);
  }

  template <typename T>
  void ModifyBit(bool val, size_t shift, zx_off_t offs) const {
    ModifyBits<T>(val, shift, 1, offs);
  }

  template <typename T>
  void SetBit(size_t shift, zx_off_t offs) const {
    ModifyBit<T>(true, shift, offs);
  }

  template <typename T>
  void ClearBit(size_t shift, zx_off_t offs) const {
    ModifyBit<T>(false, shift, offs);
  }

  uint8_t Read8(zx_off_t offs) const { return ops_->Read8(ctx_, mmio_, offs); }
  uint16_t Read16(zx_off_t offs) const { return ops_->Read16(ctx_, mmio_, offs); }
  uint32_t Read32(zx_off_t offs) const { return ops_->Read32(ctx_, mmio_, offs); }
  uint64_t Read64(zx_off_t offs) const { return ops_->Read64(ctx_, mmio_, offs); }

  // Read `size` bytes from the MmioBuffer into `buffer`. There are no access width guarantees
  // when using this operation and must only be used with devices where arbitrary access widths are
  // supported.
  void ReadBuffer(zx_off_t offs, void* buffer, size_t size) const {
    return ops_->ReadBuffer(ctx_, mmio_, offs, buffer, size);
  }

  void Write8(uint8_t val, zx_off_t offs) const { ops_->Write8(ctx_, mmio_, val, offs); }
  void Write16(uint16_t val, zx_off_t offs) const { ops_->Write16(ctx_, mmio_, val, offs); }
  void Write32(uint32_t val, zx_off_t offs) const { ops_->Write32(ctx_, mmio_, val, offs); }
  void Write64(uint64_t val, zx_off_t offs) const { ops_->Write64(ctx_, mmio_, val, offs); }

  // Write `size` bytes from `buffer` into the MmioBuffer. There are no access width guarantees
  // when using this operation and must only be used with devices where arbitrary access widths are
  // supported.
  void WriteBuffer(zx_off_t offs, const void* buffer, size_t size) const {
    ops_->WriteBuffer(ctx_, mmio_, offs, buffer, size);
  }

 protected:
  mmio_buffer_t mmio_;
  const MmioBufferOps* ops_;
  const void* ctx_;

  template <typename T>
  static constexpr std::false_type always_false{};

  static uint8_t Read8(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs) {
    return MmioRead8(GetAddr<uint8_t>(ctx, mmio, offs));
  }

  static uint16_t Read16(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs) {
    return MmioRead16(GetAddr<uint16_t>(ctx, mmio, offs));
  }

  static uint32_t Read32(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs) {
    return MmioRead32(GetAddr<uint32_t>(ctx, mmio, offs));
  }

  static uint64_t Read64(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs) {
    return MmioRead64(GetAddr<uint64_t>(ctx, mmio, offs));
  }

  static void ReadBuffer(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs, void* buffer,
                         size_t size) {
    ZX_DEBUG_ASSERT(offs + size <= mmio.size);
    return MmioReadBuffer(buffer, GetAddr<uint64_t>(ctx, mmio, offs), size);
  }

  static void Write8(const void* ctx, const mmio_buffer_t& mmio, uint8_t val, zx_off_t offs) {
    MmioWrite8(val, GetAddr<uint8_t>(ctx, mmio, offs));
    hw_mb();
  }

  static void Write16(const void* ctx, const mmio_buffer_t& mmio, uint16_t val, zx_off_t offs) {
    MmioWrite16(val, GetAddr<uint16_t>(ctx, mmio, offs));
    hw_mb();
  }

  static void Write32(const void* ctx, const mmio_buffer_t& mmio, uint32_t val, zx_off_t offs) {
    MmioWrite32(val, GetAddr<uint32_t>(ctx, mmio, offs));
    hw_mb();
  }

  static void Write64(const void* ctx, const mmio_buffer_t& mmio, uint64_t val, zx_off_t offs) {
    MmioWrite64(val, GetAddr<uint64_t>(ctx, mmio, offs));
    hw_mb();
  }

  static void WriteBuffer(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs,
                          const void* buffer, size_t size) {
    ZX_DEBUG_ASSERT(offs + size <= mmio.size);
    MmioWriteBuffer(GetAddr<uint64_t>(ctx, mmio, offs), buffer, size);
    hw_mb();
  }

  static constexpr MmioBufferOps kDefaultOps = {
      .Read8 = Read8,
      .Read16 = Read16,
      .Read32 = Read32,
      .Read64 = Read64,
      .ReadBuffer = ReadBuffer,
      .Write8 = Write8,
      .Write16 = Write16,
      .Write32 = Write32,
      .Write64 = Write64,
      .WriteBuffer = WriteBuffer,
  };

 private:
  void transfer(MmioBuffer&& other) {
    mmio_ = other.mmio_;
    ops_ = other.ops_;
    ctx_ = other.ctx_;
    memset(&other.mmio_, 0, sizeof(other.mmio_));
  }

  template <typename T>
  static MMIO_PTR volatile T* GetAddr(const void* ctx, const mmio_buffer_t& mmio, zx_off_t offs) {
    ZX_DEBUG_ASSERT(offs + sizeof(T) <= mmio.size);
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(mmio.vaddr);
    ZX_DEBUG_ASSERT(ptr);
    return reinterpret_cast<MMIO_PTR T*>(ptr + offs);
  }
};

// A sliced view that of an mmio which does not unmap on close. Must not outlive
// mmio buffer it is created from.
class MmioView : public MmioBuffer {
 public:
  MmioView(const mmio_buffer_t& mmio, zx_off_t offset, const MmioBufferOps* ops = &kDefaultOps,
           const void* ctx = nullptr)
      : MmioBuffer(
            mmio_buffer_t{
                .vaddr = static_cast<MMIO_PTR uint8_t*>(mmio.vaddr) + offset,
                .offset = mmio.offset + offset,
                .size = mmio.size - offset,
                .vmo = mmio.vmo,
            },
            ops, ctx) {
    ZX_ASSERT(offset < mmio.size);
  }

  MmioView(const mmio_buffer_t& mmio, zx_off_t offset, size_t size,
           const MmioBufferOps* ops = &kDefaultOps, const void* ctx = nullptr)
      : MmioBuffer(
            mmio_buffer_t{
                .vaddr = static_cast<MMIO_PTR uint8_t*>(mmio.vaddr) + offset,
                .offset = mmio.offset + offset,
                .size = size,
                .vmo = mmio.vmo,
            },
            ops, ctx) {
    ZX_ASSERT(size + offset <= mmio.size);
  }

  MmioView(const MmioView& mmio) : MmioBuffer(mmio.mmio_, mmio.ops_, mmio.ctx_) {}

  virtual ~MmioView() override {
    // Prevent unmap operation from occurring.
    mmio_.vmo = ZX_HANDLE_INVALID;
  }
};

// These can't be defined inside the class because they need MmioView
// to be completely defined first.

inline MmioView MmioBuffer::View(zx_off_t off) const { return MmioView(mmio_, off, ops_, ctx_); }

inline MmioView MmioBuffer::View(zx_off_t off, size_t size) const {
  return MmioView(mmio_, off, size, ops_, ctx_);
}

}  // namespace ddk

#endif  // SRC_DEVICES_LIB_MMIO_INCLUDE_LIB_MMIO_MMIO_H_
