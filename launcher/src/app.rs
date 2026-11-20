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

const TEXT: Color32 = Color32::from_rgb(236, 242, 250);
const MUTED: Color32 = Color32::from_rgb(148, 163, 184);
const PANEL: Color32 = Color32::from_rgb(9, 15, 26);
const PANEL_2: Color32 = Color32::from_rgb(16, 25, 40);
const BORDER: Color32 = Color32::from_rgb(39, 54, 75);
const ACCENT: Color32 = Color32::from_rgb(99, 179, 255);
const GREEN: Color32 = Color32::from_rgb(74, 222, 128);
const AMBER: Color32 = Color32::from_rgb(251, 191, 36);
const RED: Color32 = Color32::from_rgb(248, 113, 113);

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

fn texture_from_image(ctx: &egui::Context, name: &str, bytes: &[u8]) -> TextureHandle {
    let img = image::load_from_memory(bytes)
        .expect("embedded image")
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
    visuals.window_fill = PANEL;
    visuals.override_text_color = Some(TEXT);
    visuals.widgets.noninteractive.bg_fill = PANEL;
    visuals.widgets.noninteractive.bg_stroke = Stroke::new(1.0, BORDER);
    visuals.widgets.inactive.bg_fill = PANEL_2;
    visuals.widgets.inactive.bg_stroke = Stroke::new(1.0, BORDER);
    visuals.widgets.hovered.bg_fill = Color32::from_rgb(22, 34, 53);
    visuals.widgets.hovered.bg_stroke = Stroke::new(1.0, ACCENT.gamma_multiply(0.65));
    visuals.widgets.active.bg_fill = ACCENT;
    visuals.widgets.active.bg_stroke = Stroke::new(1.0, ACCENT);
    visuals.selection.bg_fill = ACCENT.gamma_multiply(0.30);
    visuals.selection.stroke = Stroke::new(1.0, ACCENT);
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

fn dot(ui: &mut Ui, color: Color32) {
    ui.label(RichText::new("●").color(color).size(9.0));
}

fn section_title(ui: &mut Ui, text: &str) {
    ui.label(
        RichText::new(text)
            .family(FontFamily::Name("Heading".into()))
            .size(17.0)
            .color(TEXT),
    );
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
}

impl LauncherApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        setup_fonts(&cc.egui_ctx);
        setup_style(&cc.egui_ctx);
        let bg = texture_from_image(
            &cc.egui_ctx,
            "vector_bg",
            include_bytes!("../assets/vector_bg.jpg"),
        );
        let icon = texture_from_image(&cc.egui_ctx, "icon", include_bytes!("../assets/icon.png"));

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
                    .fit_to_exact_size(Vec2::splat(46.0))
                    .corner_radius(CornerRadius::same(10)),
            );
            ui.add_space(6.0);
            ui.vertical(|ui| {
                ui.label(heading("VectorNX", 25.0));
                ui.label(RichText::new("Setup  ·  60 FPS  ·  v0.6.1").size(12.0).color(MUTED));
            });
        });
    }

    fn apk_section(&mut self, ui: &mut Ui, ctx: &egui::Context) {
        section_title(ui, "Game APK");
        ui.label(
            RichText::new("Your own legally obtained Vector 1.5.0 APK. Nothing leaves this PC.")
                .size(12.0)
                .color(MUTED),
        );
        ui.add_space(6.0);

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

        let label = if self.apk_path.is_empty() {
            "Drop your Vector APK here, or browse"
        } else {
            "Choose a different APK"
        };
        let button = egui::Button::new(
            RichText::new(label)
                .size(14.0)
                .color(if self.apk_path.is_empty() { TEXT } else { ACCENT }),
        )
        .fill(PANEL_2)
        .stroke(Stroke::new(1.0, BORDER))
        .corner_radius(CornerRadius::same(8))
        .min_size(Vec2::new(ui.available_width(), 54.0));
        if ui.add(button).clicked() {
            self.browse_apk();
        }
        if !self.apk_path.is_empty() {
            ui.add(
                egui::Label::new(
                    RichText::new(&self.apk_path)
                        .monospace()
                        .size(10.0)
                        .color(MUTED),
                )
                .truncate(),
            );
        }

        if let Some(err) = &self.apk_error {
            ui.add_space(4.0);
            ui.label(RichText::new(err).color(RED).size(11.0));
        }

        let info = self.apk_info.clone();
        if let Some(info) = info {
            ui.add_space(7.0);
            let pkg_color = if info.package_ok() { GREEN } else { RED };
            let ver_color = if info.version_ok() { GREEN } else { RED };
            let pc = info.patch.color();
            let patch_color = Color32::from_rgb(pc.0, pc.1, pc.2);
            ui.horizontal_wrapped(|ui| {
                dot(ui, pkg_color);
                ui.label(RichText::new(&info.package).size(12.0).color(pkg_color));
                ui.label(RichText::new("·").size(12.0).color(MUTED));
                dot(ui, ver_color);
                ui.label(RichText::new(&info.version_name).size(12.0).color(ver_color));
                ui.label(RichText::new("·").size(12.0).color(MUTED));
                dot(ui, patch_color);
                ui.label(
                    RichText::new(info.patch.label())
                        .size(12.0)
                        .color(patch_color),
                );
            });
            if info.version_ok() && !info.version_code_ok() {
                ui.label(
                    RichText::new("Version code is not the expected 5000131.")
                        .size(10.0)
                        .color(AMBER),
                );
            }

            egui::CollapsingHeader::new("Details")
                .id_salt("apk_details")
                .default_open(false)
                .show(ui, |ui| {
                    egui::Grid::new("apk_details_grid")
                        .num_columns(2)
                        .spacing([12.0, 5.0])
                        .show(ui, |ui| {
                            ui.label(RichText::new("SHA-256").size(10.0).color(MUTED));
                            ui.label(
                                RichText::new(&info.sha256)
                                    .monospace()
                                    .size(9.0)
                                    .color(TEXT),
                            );
                            ui.end_row();
                            ui.label(RichText::new("Signature").size(10.0).color(MUTED));
                            ui.label(
                                RichText::new(if info.signed {
                                    "signed APK"
                                } else {
                                    "unsigned / debug-style"
                                })
                                .size(10.0)
                                .color(if info.signed { TEXT } else { AMBER }),
                            );
                            ui.end_row();
                            ui.label(RichText::new("Size").size(10.0).color(MUTED));
                            ui.label(RichText::new(fmt_mb(info.size)).size(10.0).color(TEXT));
                            ui.end_row();
                            ui.label(RichText::new("License hints").size(10.0).color(MUTED));
                            ui.label(
                                RichText::new(if info.license_hits.is_empty() {
                                    "none".to_string()
                                } else {
                                    info.license_hits.join(", ")
                                })
                                .size(10.0)
                                .color(TEXT),
                            );
                            ui.end_row();
                        });
                });

            if info.patch.needs_warning() {
                ui.add_space(5.0);
                Frame::new()
                    .fill(AMBER.gamma_multiply(0.06))
                    .stroke(Stroke::new(1.0, AMBER.gamma_multiply(0.35)))
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::same(10))
                    .show(ui, |ui| {
                        ui.label(
                            RichText::new("Patched / unverified APK")
                                .size(12.0)
                                .strong()
                                .color(AMBER),
                        );
                        ui.label(
                            RichText::new(info.patch.detail())
                                .size(11.0)
                                .color(TEXT),
                        );
                        let label = if self.accept_warning {
                            "Patched APK accepted — click to undo"
                        } else {
                            "Accept patched APK warning"
                        };
                        let button = egui::Button::new(
                            RichText::new(label)
                                .size(11.0)
                                .color(if self.accept_warning { GREEN } else { AMBER }),
                        )
                        .fill(if self.accept_warning {
                            GREEN.gamma_multiply(0.10)
                        } else {
                            PANEL_2
                        })
                        .stroke(Stroke::new(
                            1.0,
                            if self.accept_warning { GREEN } else { AMBER },
                        ))
                        .corner_radius(CornerRadius::same(6));
                        if ui
                            .add_sized([ui.available_width(), 30.0], button)
                            .clicked()
                        {
                            self.accept_warning = !self.accept_warning;
                        }
                    });
            }
        }
    }

    fn destination_section(&mut self, ui: &mut Ui) {
        section_title(ui, "Install target");
        ui.label(
            RichText::new("SD card root, or the existing switch\\vector folder.")
                .size(12.0)
                .color(MUTED),
        );
        ui.add_space(4.0);
        ui.horizontal(|ui| {
            let width = (ui.available_width() - 92.0).max(120.0);
            let resp = ui.add_sized(
                [width, 32.0],
                egui::TextEdit::singleline(&mut self.settings.output)
                    .hint_text("E:\\  or  E:\\switch\\vector"),
            );
            if resp.lost_focus() {
                self.settings.save();
            }
            if ui
                .add_sized([80.0, 32.0], egui::Button::new("Browse"))
                .clicked()
            {
                self.browse_output();
            }
        });
        if !self.settings.output.trim().is_empty() {
            let resolved = install::resolve_output(Path::new(self.settings.output.trim()));
            ui.label(
                RichText::new(format!("Installs to: {}", resolved.display()))
                    .size(10.0)
                    .color(ACCENT),
            );
        }

        ui.add_space(7.0);
        ui.horizontal(|ui| {
            ui.label(RichText::new("Loader NRO").size(11.0).color(MUTED));
            if let Some((size, version)) = &self.nro_info {
                dot(ui, GREEN);
                ui.label(
                    RichText::new(format!(
                        "{}{}",
                        version.clone().unwrap_or_else(|| "version unknown".into()),
                        format!("  ·  {}", fmt_mb(*size))
                    ))
                    .size(11.0)
                    .color(GREEN),
                );
            } else if let Some(err) = &self.nro_error {
                ui.label(RichText::new(err).size(10.0).color(RED));
            }
            if ui.small_button("Browse").clicked() {
                self.browse_nro();
            }
        });

        if ui
            .checkbox(
                &mut self.settings.replace_assets,
                RichText::new("Replace existing game data (saves are never deleted)").size(11.0),
            )
            .changed()
        {
            self.settings.save();
        }
    }

    fn options_section(&mut self, ui: &mut Ui) {
        egui::CollapsingHeader::new("Options")
            .id_salt("options")
            .default_open(false)
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.vertical(|ui| {
                        ui.label(RichText::new("Language").size(10.0).color(MUTED));
                        let resp = egui::ComboBox::from_id_salt("language")
                            .width(180.0)
                            .selected_text(
                                LANGUAGES
                                    .iter()
                                    .find(|(code, _)| *code == self.settings.language)
                                    .map(|(_, name)| *name)
                                    .unwrap_or("System language"),
                            )
                            .show_ui(ui, |ui| {
                                for (code, name) in LANGUAGES {
                                    ui.selectable_value(
                                        &mut self.settings.language,
                                        (*code).to_string(),
                                        *name,
                                    );
                                }
                            });
                        if resp.response.changed() {
                            self.settings.save();
                        }
                    });
                    ui.add_space(10.0);
                    ui.vertical(|ui| {
                        ui.label(RichText::new("Resolution").size(10.0).color(MUTED));
                        let resp = egui::ComboBox::from_id_salt("resolution")
                            .width(140.0)
                            .selected_text(if self.settings.quality {
                                "1280×720"
                            } else {
                                "960×540"
                            })
                            .show_ui(ui, |ui| {
                                if ui
                                    .selectable_label(!self.settings.quality, "960×540 · Performance")
                                    .clicked()
                                {
                                    self.settings.quality = false;
                                    self.settings.save();
                                }
                                if ui
                                    .selectable_label(self.settings.quality, "1280×720 · Quality")
                                    .clicked()
                                {
                                    self.settings.quality = true;
                                    self.settings.save();
                                }
                            });
                        if resp.response.changed() {
                            self.settings.save();
                        }
                    });
                    ui.add_space(10.0);
                    ui.vertical(|ui| {
                        ui.label(RichText::new("Frame rate").size(10.0).color(MUTED));
                        ui.label(RichText::new("60 · locked").size(11.0).color(ACCENT));
                    });
                    ui.add_space(10.0);
                    ui.vertical(|ui| {
                        ui.label(RichText::new("Debug").size(10.0).color(MUTED));
                        if ui
                            .checkbox(&mut self.settings.log, "debug.log")
                            .changed()
                        {
                            self.settings.save();
                        }
                    });
                });
                ui.add_space(4.0);
                ui.label(
                    RichText::new("The asset pack is always built and verified.")
                        .size(10.0)
                        .color(MUTED),
                );
            });
    }

    fn install_section(&mut self, ui: &mut Ui) {
        if self.installing {
            ui.add(
                egui::ProgressBar::new(self.progress)
                    .show_percentage()
                    .animate(true),
            );
            ui.label(
                RichText::new(format!("{}  ·  {}", self.stage, self.detail))
                    .size(11.0)
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
                    .color(if enabled {
                        Color32::from_rgb(4, 8, 15)
                    } else {
                        MUTED
                    }),
            )
            .fill(if enabled { ACCENT } else { PANEL_2 })
            .stroke(Stroke::new(1.0, if enabled { ACCENT } else { BORDER }))
            .corner_radius(CornerRadius::same(8))
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
                ui.label(RichText::new(hint).size(10.0).color(MUTED));
            } else {
                ui.label(RichText::new("Ctrl+Enter to install").size(9.0).color(MUTED));
            }
        }

        if let Some(err) = &self.install_error {
            ui.add_space(5.0);
            ui.label(RichText::new(err).color(RED).size(11.0));
        }
        if let Some(summary) = &self.summary {
            ui.add_space(6.0);
            ui.label(RichText::new("Install complete").color(GREEN).strong());
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
                .size(10.0)
                .color(MUTED),
            );
            if ui.button("Open folder").clicked() {
                let _ = std::process::Command::new("explorer")
                    .arg(&summary.output)
                    .spawn();
            }
        }
    }

    fn footer(&mut self, ui: &mut Ui) {
        ui.add_space(4.0);
        ui.separator();
        ui.horizontal(|ui| {
            ui.label(
                RichText::new("No game data leaves this PC.")
                    .size(10.0)
                    .color(MUTED),
            );
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.hyperlink_to(
                    RichText::new("GitHub").size(10.0).color(MUTED),
                    "https://github.com/harrysof/VectorNX",
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
        let img_aspect = 1839.0 / 855.0;
        let rect_aspect = rect.width() / rect.height();
        let mut uv = egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0));
        if rect_aspect > img_aspect {
            let visible = img_aspect / rect_aspect;
            let v = (1.0 - visible) * 0.5;
            uv.min.y = v;
            uv.max.y = 1.0 - v;
        } else {
            let visible = rect_aspect / img_aspect;
            let u = (1.0 - visible) * 0.5;
            uv.min.x = u;
            uv.max.x = 1.0 - u;
        }
        painter.image(self.bg.id(), rect, uv, Color32::WHITE);
        painter.rect_filled(rect, 0.0, Color32::from_rgba_unmultiplied(3, 8, 17, 188));

        let ppp = ctx.pixels_per_point().max(0.1);
        let logical_w = rect.width() / ppp;
        let panel_w = (logical_w - 40.0).clamp(600.0, 760.0);
        let side = ((logical_w - panel_w) * 0.5).max(18.0);

        egui::ScrollArea::vertical()
            .auto_shrink([false, false])
            .show(ui, |ui| {
                ui.add_space(28.0);
                ui.horizontal(|ui| {
                    ui.add_space(side);
                    Frame::new()
                        .fill(PANEL)
                        .stroke(Stroke::new(1.0, BORDER))
                        .corner_radius(CornerRadius::same(18))
                        .inner_margin(Margin::same(24))
                        .show(ui, |ui| {
                            ui.set_width(panel_w - 48.0);
                            ui.vertical(|ui| {
                                self.header(ui);
                                ui.add_space(8.0);
                                ui.separator();
                                ui.add_space(12.0);
                                self.apk_section(ui, &ctx);
                                ui.add_space(12.0);
                                ui.separator();
                                ui.add_space(12.0);
                                self.destination_section(ui);
                                ui.add_space(6.0);
                                self.options_section(ui);
                                ui.add_space(10.0);
                                self.install_section(ui);
                                self.footer(ui);
                            });
                        });
                });
                ui.add_space(28.0);
            });
    }
}
