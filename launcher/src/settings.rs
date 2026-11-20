use std::path::PathBuf;

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub apk: String,
    pub output: String,
    pub nro: String,
    pub language: String,
    pub quality: bool,
    pub log: bool,
    pub replace_assets: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            apk: String::new(),
            output: String::new(),
            nro: String::new(),
            language: "auto".to_string(),
            quality: false,
            log: true,
            replace_assets: true,
        }
    }
}

fn path() -> PathBuf {
    let base = std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("VectorNX").join("launcher.json")
}

impl Settings {
    pub fn load() -> Self {
        let p = path();
        std::fs::read_to_string(&p)
            .ok()
            .and_then(|s| {
                let s = s.strip_prefix('\u{feff}').unwrap_or(&s);
                serde_json::from_str(s).ok()
            })
            .unwrap_or_default()
    }

    pub fn save(&self) {
        let p = path();
        if let Some(parent) = p.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        if let Ok(text) = serde_json::to_string_pretty(self) {
            let _ = std::fs::write(p, text);
        }
    }
}
