// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context, Error};
use argh::FromArgs;
use fidl_fuchsia_tpm_cr50::Cr50Marker;
use fuchsia_zircon as zx;

#[derive(FromArgs, PartialEq, Debug)]
/// A tool to interact with the Cr50 TPM.
struct Args {
    #[argh(subcommand)]
    cmd: SubCommand,
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand)]
enum SubCommand {
    Ccd(CcdSubCommand),
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "ccd")]
/// interact with case-closed debugging features.
struct CcdSubCommand {
    #[argh(subcommand)]
    cmd: CcdCommand,
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand)]
/// command to use.
enum CcdCommand {
    GetInfo(GetInfo),
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "get-info")]
/// get info about CCD.
struct GetInfo {}

#[fuchsia::component]
async fn main() {
    let args: Args = argh::from_env();
    run_cmd(args).await.unwrap_or_else(|e| {
        println!("Error while running command: {:?}", e);
    });
}

async fn run_cmd(args: Args) -> Result<(), Error> {
    let proxy = fuchsia_component::client::connect_to_protocol::<Cr50Marker>()
        .context("Connecting to firmware parameter service")?;
    match args.cmd {
        SubCommand::Ccd(CcdSubCommand { cmd: CcdCommand::GetInfo(_) }) => {
            let (rc, info) = proxy
                .ccd_get_info()
                .await
                .context("Getting info (Sending FIDL request)")?
                .map_err(zx::Status::from_raw)
                .context("Getting info (Server-side failure)")?;
            if let Some(info) = info {
                println!("CCD state: {:?}", info.state);
                println!("CCD force disabled: {}", info.force_disabled);
                println!("CCD flags: {:?}", info.flags);
                println!("CCD indicator: {:?}", info.indicator);
                println!("Capabilities:");
                println!("{:^32} {:^16} {:^16}", "CAPABILITY", "CURRENT STATE", "(DEFAULT STATE)");
                for cap in info.capabilities.iter() {
                    print!(
                        "{:^32} {:^16}",
                        format!("{:?}", cap.capability),
                        format!("{:?}", cap.current_state)
                    );
                    if cap.current_state != cap.default_state {
                        println!(" {:^16}", format!("({:?})", cap.default_state));
                    } else {
                        println!();
                    }
                }
            } else {
                println!("Error: {:?}", rc);
            }
        }
    };

    Ok(())
}