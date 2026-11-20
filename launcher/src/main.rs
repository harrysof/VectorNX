mod apk;
mod app;
mod axml;
mod install;
mod pack;
mod settings;

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::sync::mpsc;
use std::sync::Arc;

use anyhow::{bail, Context, Result};
use eframe::egui;

use app::LauncherApp;
use install::{InstallEvent, InstallOptions};

fn main() {
    dpi_aware();
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        hide_console();
        if let Err(e) = run_gui() {
            eprintln!("launcher failed: {e:#}");
            std::process::exit(1);
        }
        return;
    }
    if let Err(e) = run_cli(&args) {
        eprintln!("error: {e:#}");
        std::process::exit(1);
    }
}

#[cfg(windows)]
fn hide_console() {
    unsafe extern "system" {
        fn GetConsoleWindow() -> isize;
        fn ShowWindow(hwnd: isize, cmd: i32) -> i32;
    }
    unsafe {
        let hwnd = GetConsoleWindow();
        if hwnd != 0 {
            ShowWindow(hwnd, 0); // SW_HIDE
        }
    }
}

#[cfg(not(windows))]
fn hide_console() {}

#[cfg(windows)]
fn dpi_aware() {
    unsafe extern "system" {
        fn SetProcessDpiAwarenessContext(value: isize) -> i32;
        fn SetProcessDPIAware() -> i32;
    }
    unsafe {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        if SetProcessDpiAwarenessContext(-4) == 0 {
            SetProcessDPIAware();
        }
    }
}

#[cfg(not(windows))]
fn dpi_aware() {}

fn icon_data() -> egui::IconData {
    let img = image::load_from_memory(include_bytes!("../assets/icon.png"))
        .expect("embedded icon")
        .to_rgba8();
    egui::IconData {
        rgba: img.as_raw().clone(),
        width: img.width(),
        height: img.height(),
    }
}

fn run_gui() -> Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1400.0, 970.0])
            .with_min_inner_size([1100.0, 760.0])
            .with_icon(Arc::new(icon_data())),
        ..Default::default()
    };
    eframe::run_native(
        "VectorNX Setup",
        options,
        Box::new(|cc| Ok(Box::new(LauncherApp::new(cc)))),
    )
    .map_err(|e| anyhow::anyhow!("{e}"))
}

fn run_cli(args: &[String]) -> Result<()> {
    match args[0].as_str() {
        "--help" | "-h" => {
            println!(
                "VectorNX Setup\n\n\
                 GUI:     vectornx-launcher\n\
                 Check:   vectornx-launcher --check <apk>\n\
                 Install: vectornx-launcher --install --apk <apk> --out <sd-folder> --nro <nro>\n\
                 \x20         [--lang auto] [--width 960] [--height 540] [--log 1] [--replace 1]\n"
            );
            Ok(())
        }
        "--version" => {
            println!("VectorNX Setup 0.1.0 (loader 0.6.1-perf3)");
            Ok(())
        }
        "--check" => {
            let path = args.get(1).context("--check needs an APK path")?;
            let info = apk::inspect(&PathBuf::from(path))?;
            println!("package={}", info.package);
            println!("version_name={}", info.version_name);
            println!("version_code={}", info.version_code);
            println!("sha256={}", info.sha256);
            println!("size={}", info.size);
            println!("signed={}", info.signed);
            println!("patch={:?}", info.patch);
            println!("license_hits={}", info.license_hits.join(","));
            if !info.package_ok() {
                bail!("wrong package");
            }
            if !info.version_ok() {
                bail!("wrong version");
            }
            Ok(())
        }
        "--install" => run_install_cli(&args[1..]),
        other => bail!("unknown argument {other}; use --help"),
    }
}

fn arg_value(args: &[String], name: &str) -> Option<String> {
    args.iter()
        .position(|a| a == name)
        .and_then(|i| args.get(i + 1))
        .cloned()
}

fn run_install_cli(args: &[String]) -> Result<()> {
    let apk = PathBuf::from(arg_value(args, "--apk").context("--apk is required")?);
    let out = PathBuf::from(arg_value(args, "--out").context("--out is required")?);
    let nro = PathBuf::from(arg_value(args, "--nro").context("--nro is required")?);
    let opts = InstallOptions {
        apk,
        output: out,
        nro,
        language: arg_value(args, "--lang").unwrap_or_else(|| "auto".into()),
        width: arg_value(args, "--width")
            .and_then(|s| s.parse().ok())
            .unwrap_or(960),
        height: arg_value(args, "--height")
            .and_then(|s| s.parse().ok())
            .unwrap_or(540),
        log: arg_value(args, "--log").map(|s| s != "0").unwrap_or(true),
        replace_assets: arg_value(args, "--replace").map(|s| s != "0").unwrap_or(true),
    };

    let (tx, rx) = mpsc::channel();
    let cancel = Arc::new(AtomicBool::new(false));
    let worker = {
        let cancel = cancel.clone();
        std::thread::spawn(move || install::run(opts, tx, &cancel))
    };

    let mut last_stage = String::new();
    let mut last_printed = -1i32;
    let mut result = None;
    while let Ok(event) = rx.recv() {
        match event {
            InstallEvent::Stage {
                name,
                progress,
                detail,
            } => {
                let pct = (progress * 100.0) as i32;
                if name != last_stage || pct / 5 != last_printed / 5 {
                    println!("[{pct:3}%] {name}: {detail}");
                    last_stage = name;
                    last_printed = pct;
                }
            }
            InstallEvent::Done(summary) => {
                println!(
                    "done: {} entries, pack {:.1} MB, data {:.1} MB -> {}",
                    summary.entries,
                    summary.pack_bytes as f64 / 1_048_576.0,
                    summary.data_bytes as f64 / 1_048_576.0,
                    summary.output.display()
                );
                result = Some(Ok(()));
            }
            InstallEvent::Error(e) => {
                result = Some(Err(anyhow::anyhow!(e)));
            }
        }
    }
    let joined = worker.join().map_err(|_| anyhow::anyhow!("installer panicked"))?;
    if let Some(r) = result {
        r?;
    }
    joined.map(|_| ())
}
