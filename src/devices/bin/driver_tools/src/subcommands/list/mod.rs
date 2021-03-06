// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod args;

use {
    crate::common::{self, Device},
    anyhow::Result,
    args::ListCommand,
    bind::debugger::debug_dump::dump_bind_rules,
    fidl_fuchsia_driver_development as fdd,
    futures::join,
    std::{collections::HashSet, iter::FromIterator},
};

pub async fn list(
    cmd: ListCommand,
    driver_development_proxy: fdd::DriverDevelopmentProxy,
) -> Result<()> {
    let empty: [String; 0] = [];
    let driver_info = common::get_driver_info(&driver_development_proxy, &empty);

    let driver_info = if cmd.loaded {
        // Query devices and create a hash set of loaded drivers.
        let device_info = common::get_device_info(&driver_development_proxy, &empty);

        // Await the futures concurrently.
        let (driver_info, device_info) = join!(driver_info, device_info);

        let loaded_driver_set: HashSet<String> =
            HashSet::from_iter(device_info?.into_iter().filter_map(|device_info| {
                let device: Device = device_info.into();
                let key = match device {
                    Device::V1(ref info) => &info.0.bound_driver_libname,
                    Device::V2(ref info) => {
                        // DFv2 nodes do not have a bound driver libname so the
                        // bound driver URL is selected instead.
                        &info.0.bound_driver_url
                    }
                };
                match key {
                    Some(key) => Some(key.to_owned()),
                    None => None,
                }
            }));

        // Filter the driver list by the hash set.
        driver_info?
            .into_iter()
            .filter(|driver| {
                let mut loaded = false;
                if let Some(ref libname) = driver.libname {
                    if loaded_driver_set.contains(libname) {
                        loaded = true;
                    }
                }
                if let Some(ref url) = driver.url {
                    if loaded_driver_set.contains(url) {
                        loaded = true
                    }
                }
                loaded
            })
            .collect()
    } else {
        driver_info.await?
    };

    if cmd.verbose {
        for driver in driver_info {
            if let Some(name) = driver.name {
                println!("{0: <10}: {1}", "Name", name);
            }
            if let Some(url) = driver.url {
                println!("{0: <10}: {1}", "URL", url);
            }
            if let Some(libname) = driver.libname {
                println!("{0: <10}: {1}", "Driver", libname);
            }
            match driver.bind_rules {
                Some(fdd::BindRulesBytecode::BytecodeV1(bytecode)) => {
                    println!("{0: <10}: {1}", "Bytecode Version", 1);
                    println!("{0: <10}({1} bytes): {2:?}", "Bytecode:", bytecode.len(), bytecode);
                }
                Some(fdd::BindRulesBytecode::BytecodeV2(bytecode)) => {
                    println!("{0: <10}: {1}", "Bytecode Version", 2);
                    println!("{0: <10}({1} bytes): ", "Bytecode:", bytecode.len());
                    match dump_bind_rules(bytecode.clone()) {
                        Ok(bytecode_dump) => println!("{}", bytecode_dump),
                        Err(err) => {
                            println!("  Issue parsing bytecode \"{}\": {:?}", err, bytecode);
                        }
                    }
                }
                _ => println!("{0: <10}: {1}", "Bytecode Version", "Unknown"),
            }
            println!();
        }
    } else {
        for driver in driver_info {
            if let Some(name) = driver.name {
                let libname_or_url = driver.libname.or(driver.url).unwrap_or("".to_string());
                println!("{:<20}: {}", name, libname_or_url);
            } else {
                let url_or_libname = driver.url.or(driver.libname).unwrap_or("".to_string());
                println!("{}", url_or_libname);
            }
        }
    }
    Ok(())
}
