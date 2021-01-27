// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {crate::fidl::FidlIr, anyhow::Error, std::io};

pub use self::dummy_c::DummyCBackend;

mod dummy_c;

pub trait Backend<W: io::Write> {
    fn codegen(&mut self, ir: FidlIr) -> Result<(), Error>;
}
