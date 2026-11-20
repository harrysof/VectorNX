use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver};
use std::sync::Arc;
use std::time::Duration;

use eframe::egui::{
    self, Align, Color32, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, Frame,
    Layout, Margin, RichText, Stroke, TextureHandle, TextureOptions, Ui, Vec2,
};

use crate::apk::{self, ApkInfo};
use crate::install::{self, InstallEvent, InstallOptions, InstallSummary};
use crate::settings::Settings;

const TEXT: Color32 = Color32::from_rgb(232, 238, 245);
const MUTED: Color32 = Color32::from_rgb(139, 152, 168);
const CARD: Color32 = Color32::from_rgb(19, 25, 34);
const CARD_2: Color32 = Color32::from_rgb(24, 31, 42);
const BORDER: Color32 = Color32::from_rgb(39, 51, 66);
const ORANGE: Color32 = Color32::from_rgb(255, 106, 61);
const TEAL: Color32 = Color32::from_rgb(45, 212, 191);
const GREEN: Color32 = Color32::from_rgb(74, 222, 128);
const AMBER: Color32 = Color32::from_rgb(251, 191, 36);
const RED: Color32 = Color32::from_rgb(248, 113, 113);
const BLUE: Color32 = Color32::from_rgb(125, 211, 252);

const LANGUAGES: &[(&str, &str)] = &[
    ("auto", "System language"),
    ("en", "English"),
    ("ru", "Русский"),
    ("de", "Deutsch"),
    ("fr", "Français"),
    ("es", "Español"),
    ("it", "Italiano"),
    ("pt", "Português"),
    ("tr", "Türkçe"),
    ("ja", "日本語"),
    ("ko", "한국어"),
    ("zh_CN", "简体中文"),
];

fn texture_from_png(ctx: &egui::Context, name: &str, bytes: &[u8]) -> TextureHandle {
    let img = image::load_from_memory(bytes)
        .expect("embedded PNG")
        .to_rgba8();
    let size = [img.width() as usize, img.height() as usize];
    let pixels = img.into_raw();
    let color = egui::ColorImage::from_rgba_unmultiplied(size, &pixels);
    ctx.load_texture(name, color, TextureOptions::LINEAR)
}

fn setup_fonts(ctx: &egui::Context) {
    let mut fonts = FontDefinitions::default();
    if let Ok(data) = std::fs::read("C:\\Windows\\Fonts\\segoeui.ttf") {
        fonts
            .font_data
            .insert("segoe".into(), FontData::from_owned(data).into());
        fonts
            .families
            .entry(FontFamily::Proportional)
            .or_default()
            .insert(0, "segoe".into());
    }
    if let Ok(data) = std::fs::read("C:\\Windows\\Fonts\\segoeuib.ttf") {
        fonts
            .font_data
            .insert("segoe-bold".into(), FontData::from_owned(data).into());
        fonts.families.insert(
            FontFamily::Name("Heading".into()),
            vec!["segoe-bold".into(), "segoe".into()],
        );
    }
    ctx.set_fonts(fonts);
}

fn setup_style(ctx: &egui::Context) {
    let mut visuals = egui::Visuals::dark();
    visuals.panel_fill = Color32::TRANSPARENT;
    visuals.window_fill = CARD;
    visuals.override_text_color = Some(TEXT);
    visuals.widgets.noninteractive.bg_fill = CARD;
    visuals.widgets.noninteractive.bg_stroke = Stroke::new(1.0, BORDER);
    visuals.widgets.inactive.bg_fill = CARD_2;
    visuals.widgets.inactive.bg_stroke = Stroke::new(1.0, BORDER);
    visuals.widgets.hovered.bg_fill = Color32::from_rgb(33, 43, 57);
    visuals.widgets.hovered.bg_stroke = Stroke::new(1.0, ORANGE.gamma_multiply(0.7));
    visuals.widgets.active.bg_fill = ORANGE;
    visuals.widgets.active.bg_stroke = Stroke::new(1.0, ORANGE);
    visuals.selection.bg_fill = ORANGE.gamma_multiply(0.35);
    visuals.selection.stroke = Stroke::new(1.0, ORANGE);
    visuals.window_corner_radius = CornerRadius::same(14);
    ctx.set_visuals(visuals);

    ctx.all_styles_mut(|style| {
        style.spacing.item_spacing = Vec2::new(10.0, 10.0);
        style.spacing.button_padding = Vec2::new(14.0, 9.0);
        style.text_styles.insert(
            egui::TextStyle::Body,
            FontId::new(15.0, FontFamily::Proportional),
        );
        style.text_styles.insert(
            egui::TextStyle::Button,
            FontId::new(15.0, FontFamily::Proportional),
        );
        style.text_styles.insert(
            egui::TextStyle::Small,
            FontId::new(12.0, FontFamily::Proportional),
        );
    });
}

fn heading(text: impl Into<String>, size: f32) -> RichText {
    RichText::new(text)
        .family(FontFamily::Name("Heading".into()))
        .size(size)
        .color(TEXT)
}

fn card(ui: &mut Ui, add: impl FnOnce(&mut Ui)) {
    Frame::new()
        .fill(CARD)
        .stroke(Stroke::new(1.0, BORDER))
        .corner_radius(CornerRadius::same(14))
        .inner_margin(Margin::same(18))
        .show(ui, |ui| {
            add(ui);
        });
}

fn pill(ui: &mut Ui, text: &str, color: Color32) {
    Frame::new()
        .fill(color.gamma_multiply(0.14))
        .stroke(Stroke::new(1.0, color.gamma_multiply(0.45)))
        .corner_radius(CornerRadius::same(255))
        .inner_margin(Margin::symmetric(10, 4))
        .show(ui, |ui| {
            ui.label(RichText::new(text).size(12.0).color(color));
        });
}

fn status_row(ui: &mut Ui, label: &str, value: &str, color: Color32) {
    ui.horizontal(|ui| {
        ui.label(RichText::new("●").color(color).size(12.0));
        ui.add_sized(
            [120.0, 18.0],
            egui::Label::new(RichText::new(label).color(MUTED).size(13.0)),
        );
        ui.label(RichText::new(value).color(color).size(13.0));
    });
}

fn default_nro() -> String {
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            for candidate in [dir.join("vector_nx.nro"), dir.join("payload/vector_nx.nro")] {
                if candidate.is_file() {
                    return candidate.to_string_lossy().to_string();
                }
            }
        }
    }
    let cwd = PathBuf::from("vector_nx.nro");
    if cwd.is_file() {
        return cwd.to_string_lossy().to_string();
    }
    String::new()
}

fn fmt_mb(bytes: u64) -> String {
    format!("{:.1} MB", bytes as f64 / 1_048_576.0)
}

pub struct LauncherApp {
    settings: Settings,
    apk_path: String,
    apk_info: Option<ApkInfo>,
    apk_error: Option<String>,
    validating: bool,
    validate_rx: Option<Receiver<Result<ApkInfo, String>>>,
    nro_info: Option<(u64, Option<String>)>,
    nro_error: Option<String>,
    installing: bool,
    install_rx: Option<Receiver<InstallEvent>>,
    cancel: Arc<AtomicBool>,
    progress: f32,
    stage: String,
    detail: String,
    summary: Option<InstallSummary>,
    install_error: Option<String>,
    accept_warning: bool,
    bg: TextureHandle,
    icon: TextureHandle,
    saved_warning: bool,
}

impl LauncherApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        setup_fonts(&cc.egui_ctx);
        setup_style(&cc.egui_ctx);
        let bg = texture_from_png(
            &cc.egui_ctx,
            "background",
            include_bytes!("../assets/background.png"),
        );
        let icon = texture_from_png(&cc.egui_ctx, "icon", include_bytes!("../assets/icon.png"));

        let mut settings = Settings::load();
        if settings.nro.is_empty() {
            settings.nro = default_nro();
        }
        let mut app = Self {
            apk_path: settings.apk.clone(),
            settings,
            apk_info: None,
            apk_error: None,
            validating: false,
            validate_rx: None,
            nro_info: None,
            nro_error: None,
            installing: false,
            install_rx: None,
            cancel: Arc::new(AtomicBool::new(false)),
            progress: 0.0,
            stage: "Ready".to_string(),
            detail: String::new(),
            summary: None,
            install_error: None,
            accept_warning: false,
            bg,
            icon,
            saved_warning: false,
        };
        app.refresh_nro();
        if !app.apk_path.is_empty() && Path::new(&app.apk_path).is_file() {
            app.start_validate();
        }
        app
    }

    fn refresh_nro(&mut self) {
        self.nro_info = None;
        self.nro_error = None;
        if self.settings.nro.trim().is_empty() {
            self.nro_error = Some("Place vector_nx.nro next to the launcher, or browse for it.".into());
            return;
        }
        match apk::inspect_nro(Path::new(&self.settings.nro)) {
            Ok(info) => self.nro_info = Some(info),
            Err(e) => self.nro_error = Some(format!("{e:#}")),
        }
    }

    fn start_validate(&mut self) {
        self.accept_warning = false;
        self.saved_warning = false;
        self.summary = None;
        self.install_error = None;
        self.apk_info = None;
        self.apk_error = None;
        if self.apk_path.trim().is_empty() {
            return;
        }
        self.settings.apk = self.apk_path.clone();
        self.settings.save();
        self.validating = true;
        let (tx, rx) = mpsc::channel();
        self.validate_rx = Some(rx);
        let path = PathBuf::from(&self.apk_path);
        std::thread::spawn(move || {
            let result = apk::inspect(&path).map_err(|e| format!("{e:#}"));
            let _ = tx.send(result);
        });
    }

    fn poll_validate(&mut self, ctx: &egui::Context) {
        if let Some(rx) = &self.validate_rx {
            match rx.try_recv() {
                Ok(Ok(info)) => {
                    self.apk_info = Some(info);
                    self.apk_error = None;
                    self.validating = false;
                    self.validate_rx = None;
                }
                Ok(Err(e)) => {
                    self.apk_error = Some(e);
                    self.apk_info = None;
                    self.validating = false;
                    self.validate_rx = None;
                }
                Err(mpsc::TryRecvError::Empty) => {
                    ctx.request_repaint_after(Duration::from_millis(100));
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.apk_error = Some("APK inspection stopped unexpectedly".into());
                    self.validating = false;
                    self.validate_rx = None;
                }
            }
        }
    }

    fn start_install(&mut self) {
        if self.installing {
            return;
        }
        let Some(info) = &self.apk_info else { return };
        if !info.ready_to_install() {
            return;
        }
        if info.patch.needs_warning() && !self.accept_warning {
            return;
        }
        let output = PathBuf::from(self.settings.output.trim());
        let nro = PathBuf::from(self.settings.nro.trim());
        let width = if self.settings.quality { 1280 } else { 960 };
        let height = if self.settings.quality { 720 } else { 540 };
        let opts = InstallOptions {
            apk: PathBuf::from(&self.apk_path),
            output,
            nro,
            language: self.settings.language.clone(),
            width,
            height,
            log: self.settings.log,
            replace_assets: self.settings.replace_assets,
        };
        self.settings.save();
        self.summary = None;
        self.install_error = None;
        self.installing = true;
        self.progress = 0.0;
        self.stage = "Starting".to_string();
        self.detail = String::new();
        self.cancel = Arc::new(AtomicBool::new(false));
        let cancel = self.cancel.clone();
        let (tx, rx) = mpsc::channel();
        self.install_rx = Some(rx);
        std::thread::spawn(move || {
            let _ = install::run(opts, tx, &cancel);
        });
    }

    fn poll_install(&mut self, ctx: &egui::Context) {
        let mut done = false;
        if let Some(rx) = &self.install_rx {
            loop {
                match rx.try_recv() {
                    Ok(InstallEvent::Stage {
                        name,
                        progress,
                        detail,
                    }) => {
                        self.stage = name;
                        self.progress = progress;
                        self.detail = detail;
                    }
                    Ok(InstallEvent::Done(summary)) => {
                        self.summary = Some(summary);
                        self.installing = false;
                        self.progress = 1.0;
                        self.stage = "Installed".to_string();
                        done = true;
                        break;
                    }
                    Ok(InstallEvent::Error(e)) => {
                        self.install_error = Some(e);
                        self.installing = false;
                        done = true;
                        break;
                    }
                    Err(mpsc::TryRecvError::Empty) => break,
                    Err(mpsc::TryRecvError::Disconnected) => {
                        if self.installing {
                            self.install_error = Some("installer stopped unexpectedly".into());
                            self.installing = false;
                        }
                        done = true;
                        break;
                    }
                }
            }
        }
        if done {
            self.install_rx = None;
        }
        if self.installing {
            ctx.request_repaint_after(Duration::from_millis(100));
        }
    }

    fn can_install(&self) -> bool {
        let Some(info) = &self.apk_info else {
            return false;
        };
        info.ready_to_install()
            && (!info.patch.needs_warning() || self.accept_warning)
            && !self.settings.output.trim().is_empty()
            && self.nro_error.is_none()
            && !self.installing
    }

    fn browse_apk(&mut self) {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("Android APK", &["apk"])
            .pick_file()
        {
            self.apk_path = path.to_string_lossy().to_string();
            self.start_validate();
        }
    }

    fn browse_output(&mut self) {
        if let Some(path) = rfd::FileDialog::new().pick_folder() {
            self.settings.output = path.to_string_lossy().to_string();
            self.settings.save();
        }
    }

    fn browse_nro(&mut self) {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("Switch NRO", &["nro"])
            .pick_file()
        {
            self.settings.nro = path.to_string_lossy().to_string();
            self.settings.save();
            self.refresh_nro();
        }
    }

    fn header(&mut self, ui: &mut Ui) {
        ui.horizontal(|ui| {
            ui.add(
                egui::Image::new(&self.icon)
                    .fit_to_exact_size(Vec2::splat(54.0))
                    .corner_radius(CornerRadius::same(12)),
            );
            ui.add_space(4.0);
            ui.vertical(|ui| {
                ui.label(heading("VectorNX", 27.0));
                ui.label(
                    RichText::new("SD card setup and performance installer")
                        .size(13.0)
                        .color(MUTED),
                );
            });
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                pill(ui, "v0.6.1 · 60 fps locked", TEAL);
            });
        });
    }

    fn apk_card(&mut self, ui: &mut Ui, ctx: &egui::Context) {
        card(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(heading("Game APK", 18.0));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if self.validating {
                        ui.spinner();
                        ui.label(RichText::new("Checking…").color(MUTED).size(12.0));
                    } else if let Some(info) = &self.apk_info {
                        let c = info.patch.color();
                        pill(ui, info.patch.label(), Color32::from_rgb(c.0, c.1, c.2));
                    }
                });
            });
            ui.label(
                RichText::new("Your own legally obtained Vector APK. It is read locally; nothing is uploaded.")
                    .size(12.0)
                    .color(MUTED),
            );
            ui.add_space(4.0);

            let dropped = ctx.input(|i| {
                i.raw
                    .dropped_files
                    .first()
                    .map(|f| f.path().to_path_buf())
            });
            if let Some(path) = dropped {
                if path.extension().is_some_and(|e| e.eq_ignore_ascii_case("apk")) {
                    self.apk_path = path.to_string_lossy().to_string();
                    self.start_validate();
                }
            }

            let text = if self.apk_path.is_empty() {
                "Drop Vector 1.5.0 APK here, or click to browse"
            } else {
                "Click to choose a different APK"
            };
            let button = egui::Button::new(
                RichText::new(text)
                    .size(15.0)
                    .color(if self.apk_path.is_empty() { TEXT } else { TEAL }),
            )
            .fill(CARD_2)
            .stroke(Stroke::new(1.0, BORDER))
            .corner_radius(CornerRadius::same(10))
            .min_size(Vec2::new(ui.available_width(), 62.0));
            if ui.add(button).clicked() {
                self.browse_apk();
            }

            if !self.apk_path.is_empty() {
                ui.add_space(2.0);
                ui.add(
                    egui::Label::new(
                        RichText::new(&self.apk_path)
                            .monospace()
                            .size(11.0)
                            .color(MUTED),
                    )
                    .truncate(),
                );
            }

            if let Some(err) = &self.apk_error {
                ui.add_space(6.0);
                Frame::new()
                    .fill(RED.gamma_multiply(0.10))
                    .stroke(Stroke::new(1.0, RED.gamma_multiply(0.4)))
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::same(10))
                    .show(ui, |ui| {
                        ui.label(RichText::new(err).color(RED).size(12.0));
                    });
            }

            if let Some(info) = &self.apk_info {
                ui.add_space(6.0);
                let pkg_color = if info.package_ok() { GREEN } else { RED };
                let ver_color = if info.version_ok() { GREEN } else { RED };
                status_row(
                    ui,
                    "Package",
                    &info.package,
                    pkg_color,
                );
                status_row(
                    ui,
                    "Version",
                    &format!(
                        "{}  (code {}){}",
                        info.version_name,
                        info.version_code,
                        if info.version_code_ok() { "" } else { "  · unexpected code" }
                    ),
                    if info.version_ok() {
                        if info.version_code_ok() { ver_color } else { AMBER }
                    } else {
                        RED
                    },
                );
                let pc = info.patch.color();
                status_row(
                    ui,
                    "Patch status",
                    info.patch.label(),
                    Color32::from_rgb(pc.0, pc.1, pc.2),
                );
                status_row(
                    ui,
                    "Signature",
                    if info.signed { "signed APK" } else { "unsigned / debug-style" },
                    if info.signed { TEAL } else { AMBER },
                );
                ui.horizontal(|ui| {
                    ui.label(RichText::new("●").color(BLUE).size(12.0));
                    ui.add_sized(
                        [120.0, 18.0],
                        egui::Label::new(RichText::new("SHA-256").color(MUTED).size(13.0)),
                    );
                    ui.label(
                        RichText::new(format!("{}…", &info.sha256[..16.min(info.sha256.len())]))
                            .monospace()
                            .size(12.0)
                            .color(BLUE),
                    );
                });
                ui.label(
                    RichText::new(fmt_mb(info.size))
                        .size(11.0)
                        .color(MUTED),
                );

                if !info.package_ok() {
                    ui.add_space(4.0);
                    ui.label(
                        RichText::new(format!(
                            "Wrong package. This port expects {}.",
                            apk::VECTOR_PACKAGE
                        ))
                        .color(RED)
                        .size(12.0),
                    );
                }
                if !info.version_ok() {
                    ui.add_space(4.0);
                    ui.label(
                        RichText::new(format!(
                            "Wrong version. This port is built for {} only.",
                            apk::VECTOR_VERSION
                        ))
                        .color(RED)
                        .size(12.0),
                    );
                }

                if info.patch.needs_warning() {
                    ui.add_space(8.0);
                    Frame::new()
                        .fill(AMBER.gamma_multiply(0.08))
                        .stroke(Stroke::new(1.0, AMBER.gamma_multiply(0.35)))
                        .corner_radius(CornerRadius::same(10))
                        .inner_margin(Margin::same(12))
                        .show(ui, |ui| {
                            ui.label(
                                RichText::new("⚠  Patched / unverified APK")
                                    .color(AMBER)
                                    .strong()
                                    .size(13.0),
                            );
                            ui.label(
                                RichText::new(info.patch.detail())
                                    .color(TEXT)
                                    .size(12.0),
                            );
                            let label = if self.accept_warning {
                                "✓  Patched APK accepted — click to undo"
                            } else {
                                "Accept patched APK warning"
                            };
                            let button = egui::Button::new(
                                RichText::new(label)
                                    .size(12.0)
                                    .color(if self.accept_warning { GREEN } else { AMBER }),
                            )
                            .fill(if self.accept_warning {
                                GREEN.gamma_multiply(0.12)
                            } else {
                                CARD_2
                            })
                            .stroke(Stroke::new(
                                1.0,
                                if self.accept_warning { GREEN } else { AMBER },
                            ))
                            .corner_radius(CornerRadius::same(8));
                            if ui
                                .add_sized(
                                    [ui.available_width(), 34.0],
                                    button,
                                )
                                .clicked()
                            {
                                self.accept_warning = !self.accept_warning;
                            }
                            ui.label(
                                RichText::new("Required before installing a patched/unverified APK.")
                                    .size(10.0)
                                    .color(MUTED),
                            );
                        });
                }
            }
        });
    }

    fn target_card(&mut self, ui: &mut Ui) {
        card(ui, |ui| {
            ui.label(heading("Install target", 18.0));
            ui.label(
                RichText::new("Choose the SD card root (e.g. E:\\) or an existing switch\\vector folder.")
                    .size(12.0)
                    .color(MUTED),
            );
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                let width = (ui.available_width() - 100.0).max(120.0);
                let resp = ui.add_sized(
                    [width, 30.0],
                    egui::TextEdit::singleline(&mut self.settings.output)
                        .hint_text("E:\\  or  E:\\switch\\vector"),
                );
                if resp.lost_focus() {
                    self.settings.save();
                }
                if ui.button("Browse…").clicked() {
                    self.browse_output();
                }
            });
            if !self.settings.output.trim().is_empty() {
                let resolved = install::resolve_output(Path::new(self.settings.output.trim()));
                ui.label(
                    RichText::new(format!("Installs to: {}", resolved.display()))
                        .size(11.0)
                        .color(TEAL),
                );
            }

            ui.add_space(6.0);
            ui.horizontal(|ui| {
                ui.label(RichText::new("Loader NRO").color(MUTED).size(13.0));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if ui.small_button("Browse…").clicked() {
                        self.browse_nro();
                    }
                });
            });
            if let Some((size, version)) = &self.nro_info {
                status_row(
                    ui,
                    "vector_nx.nro",
                    &format!(
                        "{}{}",
                        version.clone().unwrap_or_else(|| "version unknown".into()),
                        format!("  ·  {}", fmt_mb(*size))
                    ),
                    GREEN,
                );
            } else if let Some(err) = &self.nro_error {
                ui.label(RichText::new(err).color(RED).size(12.0));
            }

            ui.add_space(8.0);
            ui.horizontal(|ui| {
                if ui
                    .checkbox(&mut self.settings.replace_assets, "Replace existing game data")
                    .changed()
                {
                    self.settings.save();
                }
                ui.label(
                    RichText::new("(save files are never deleted)")
                        .size(11.0)
                        .color(MUTED),
                );
            });


        });
    }

    fn action_card(&mut self, ui: &mut Ui) {
        card(ui, |ui| {
            ui.label(heading("Install", 18.0));
            ui.add_space(2.0);
            if self.installing {
                ui.add(
                    egui::ProgressBar::new(self.progress)
                        .show_percentage()
                        .animate(true),
                );
                ui.label(
                    RichText::new(format!("{}  ·  {}", self.stage, self.detail))
                        .size(12.0)
                        .color(MUTED),
                );
                if ui.button("Cancel").clicked() {
                    self.cancel.store(true, Ordering::Relaxed);
                    self.stage = "Cancelling…".into();
                }
            } else {
                let enabled = self.can_install();
                let button = egui::Button::new(
                    RichText::new("Install to SD Card")
                        .family(FontFamily::Name("Heading".into()))
                        .size(17.0)
                        .color(if enabled { Color32::from_rgb(20, 12, 8) } else { MUTED }),
                )
                .fill(if enabled { ORANGE } else { CARD_2 })
                .stroke(Stroke::new(1.0, if enabled { ORANGE } else { BORDER }))
                .corner_radius(CornerRadius::same(10))
                .min_size(Vec2::new(ui.available_width(), 46.0));
                if ui.add_enabled(enabled, button).clicked() {
                    self.start_install();
                }
                if !enabled {
                    let hint = if self.apk_info.is_none() {
                        "Select a valid 1.5.0 APK."
                    } else if self.settings.output.trim().is_empty() {
                        "Choose an SD card folder."
                    } else if self.nro_error.is_some() {
                        "Add vector_nx.nro."
                    } else if self
                        .apk_info
                        .as_ref()
                        .is_some_and(|i| i.patch.needs_warning())
                        && !self.accept_warning
                    {
                        "Accept the patched-APK warning."
                    } else {
                        "The APK must be Vector 1.5.0."
                    };
                    ui.label(RichText::new(hint).size(11.0).color(MUTED));
                }
                ui.label(
                    RichText::new("Ctrl+Enter: install")
                        .size(10.0)
                        .color(MUTED),
                );
            }
            if let Some(err) = &self.install_error {
                ui.add_space(6.0);
                ui.label(RichText::new(err).color(RED).size(12.0));
            }
            if let Some(summary) = &self.summary {
                ui.add_space(6.0);
                ui.label(RichText::new("✓  Install complete").color(GREEN).strong());
                ui.label(
                    RichText::new(format!(
                        "{} assets  ·  pack {}{}",
                        summary.entries,
                        fmt_mb(summary.pack_bytes),
                        summary
                            .nro_version
                            .as_ref()
                            .map(|v| format!("  ·  loader {v}"))
                            .unwrap_or_default()
                    ))
                    .size(11.0)
                    .color(MUTED),
                );
                if ui.button("Open folder").clicked() {
                    let _ = std::process::Command::new("explorer")
                        .arg(&summary.output)
                        .spawn();
                }
            }
        });
    }

    fn settings_card(&mut self, ui: &mut Ui) {
        card(ui, |ui| {
            ui.label(heading("Settings", 18.0));
            ui.add_space(2.0);
            ui.label(RichText::new("Written to config.txt").size(12.0).color(MUTED));
            ui.add_space(4.0);

            ui.label(RichText::new("Language").color(MUTED).size(12.0));
            egui::ComboBox::from_id_salt("language")
                .selected_text(
                    LANGUAGES
                        .iter()
                        .find(|(code, _)| *code == self.settings.language)
                        .map(|(_, name)| *name)
                        .unwrap_or("System language"),
                )
                .width(ui.available_width())
                .show_ui(ui, |ui| {
                    for (code, name) in LANGUAGES {
                        ui.selectable_value(&mut self.settings.language, (*code).to_string(), *name);
                    }
                });

            ui.add_space(6.0);
            ui.label(RichText::new("Render size").color(MUTED).size(12.0));
            ui.vertical(|ui| {
                if ui
                    .selectable_label(!self.settings.quality, "Performance · 960×540")
                    .clicked()
                {
                    self.settings.quality = false;
                    self.settings.save();
                }
                if ui
                    .selectable_label(self.settings.quality, "Quality · 1280×720")
                    .clicked()
                {
                    self.settings.quality = true;
                    self.settings.save();
                }
            });

            ui.add_space(6.0);
            status_row(ui, "Frame rate", "60 fps · hard-locked", TEAL);
            ui.add_space(2.0);
            if ui
                .checkbox(&mut self.settings.log, "Write debug.log")
                .changed()
            {
                self.settings.save();
            }
            ui.label(
                RichText::new("Useful for testing; turn off for normal play.")
                    .size(11.0)
                    .color(MUTED),
            );
            ui.add_space(8.0);
            Frame::new()
                .fill(CARD_2)
                .corner_radius(CornerRadius::same(8))
                .inner_margin(Margin::same(10))
                .show(ui, |ui| {
                    ui.label(
                        RichText::new("The asset pack is always built and verified. It is not optional.")
                            .size(11.0)
                            .color(TEXT),
                    );
                });
        });
    }
}

impl eframe::App for LauncherApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.poll_validate(&ctx);
        self.poll_install(&ctx);
        if ctx.input(|i| i.modifiers.command && i.key_pressed(egui::Key::Enter)) {
            if self.can_install() {
                self.start_install();
            } else if self
                .apk_info
                .as_ref()
                .is_some_and(|info| info.patch.needs_warning())
            {
                self.accept_warning = true;
            }
        }

        let rect = ctx.viewport_rect();
        let painter = ui.painter();
        painter.image(
            self.bg.id(),
            rect,
            egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
            Color32::WHITE,
        );
        painter.rect_filled(rect, 0.0, Color32::from_rgba_unmultiplied(5, 8, 12, 190));

        ui.add_space(14.0);
        const CONTENT_W: f32 = 1040.0;
        const LEFT_W: f32 = 640.0;
        const RIGHT_W: f32 = 390.0;
        let ppp = ui.ctx().pixels_per_point().max(0.1);
        let logical_w = ui.available_width() / ppp;
        let side = ((logical_w - CONTENT_W) * 0.5).max(22.0);
        ui.horizontal(|ui| {
            ui.add_space(side);
            ui.vertical(|ui| {
                ui.set_min_width(CONTENT_W);
                ui.set_max_width(CONTENT_W);
                self.header(ui);
            });
        });
        ui.add_space(12.0);
        egui::ScrollArea::vertical()
            .auto_shrink([false, false])
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.add_space(side);
                    ui.vertical(|ui| {
                        ui.set_min_width(LEFT_W);
                        ui.set_max_width(LEFT_W);
                        self.apk_card(ui, &ctx);
                        ui.add_space(10.0);
                        self.target_card(ui);
                    });
                    ui.add_space(10.0);
                    ui.vertical(|ui| {
                        ui.set_min_width(RIGHT_W);
                        ui.set_max_width(RIGHT_W);
                        self.action_card(ui);
                        ui.add_space(10.0);
                        self.settings_card(ui);
                        ui.add_space(10.0);
                        Frame::new()
                            .fill(CARD)
                            .stroke(Stroke::new(1.0, BORDER))
                            .corner_radius(CornerRadius::same(14))
                            .inner_margin(Margin::same(14))
                            .show(ui, |ui| {
                                ui.label(
                                    RichText::new("What happens")
                                        .family(FontFamily::Name("Heading".into()))
                                        .size(15.0),
                                );
                                for line in [
                                    "1. Validate package and 1.5.0 version",
                                    "2. Extract arm64 libraries and Unity data",
                                    "3. Build + verify assets.nxpack (mandatory)",
                                    "4. Install the loader NRO and config",
                                ] {
                                    ui.label(RichText::new(line).size(12.0).color(MUTED));
                                }
                                ui.add_space(6.0);
                                ui.hyperlink_to(
                                    RichText::new("github.com/harrysof/VectorNX")
                                        .size(11.0)
                                        .color(MUTED),
                                    "https://github.com/harrysof/VectorNX",
                                );
                            });
                    });
                });
                ui.add_space(18.0);
            });
    }
}
