//! APK inspection: package/version, signature hints, patch heuristic, hash.

use std::fs::File;
use std::io::Read;
use std::path::Path;

use anyhow::{anyhow, bail, Context, Result};
use sha2::{Digest, Sha256};
use zip::ZipArchive;

use crate::axml::ManifestInfo;

pub const VECTOR_PACKAGE: &str = "com.nekki.vector.paid";
pub const VECTOR_VERSION: &str = "1.5.0";
pub const VECTOR_VERSION_CODE: i64 = 5_000_131;
pub const KNOWN_DEV_PATCHED_SHA256: &str =
    "72EB91A77604D53BF896458B1D8EC6927EC1A8ED929DA4DB964C5004A243FFAD";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PatchStatus {
    KnownPatched,
    LikelyPatched,
    PossiblyStock,
    Unknown,
}

impl PatchStatus {
    pub fn label(self) -> &'static str {
        match self {
            Self::KnownPatched => "Known patched development APK",
            Self::LikelyPatched => "Patched / modified (likely)",
            Self::PossiblyStock => "Possible stock APK (license code found)",
            Self::Unknown => "Unverified",
        }
    }

    pub fn detail(self) -> &'static str {
        match self {
            Self::KnownPatched => {
                "This matches the patched 1.5.0 development copy. It is the expected input."
            }
            Self::LikelyPatched => {
                "No Play licensing strings were found. This is probably patched or stripped. \
                 Continue only with a legally obtained copy of your own APK."
            }
            Self::PossiblyStock => {
                "Play licensing/Google Play code is present. This may be the stock APK, which \
                 usually fails license checks. A patched copy of your own APK is recommended."
            }
            Self::Unknown => {
                "The launcher could not classify this APK. Continue at your own risk."
            }
        }
    }

    pub fn needs_warning(self) -> bool {
        !matches!(self, Self::KnownPatched)
    }

    pub fn color(self) -> (u8, u8, u8) {
        match self {
            Self::KnownPatched => (74, 222, 128),
            Self::LikelyPatched => (251, 191, 36),
            Self::PossiblyStock => (251, 146, 60),
            Self::Unknown => (148, 163, 184),
        }
    }
}

#[derive(Debug, Clone)]
pub struct ApkInfo {
    
    pub package: String,
    pub version_name: String,
    pub version_code: i64,
    pub sha256: String,
    pub size: u64,
    pub patch: PatchStatus,
    pub license_hits: Vec<String>,
    pub signed: bool,
}

impl ApkInfo {
    pub fn package_ok(&self) -> bool {
        self.package == VECTOR_PACKAGE
    }

    pub fn version_ok(&self) -> bool {
        self.version_name == VECTOR_VERSION
    }

    pub fn version_code_ok(&self) -> bool {
        self.version_code == VECTOR_VERSION_CODE
    }

    pub fn ready_to_install(&self) -> bool {
        self.package_ok() && self.version_ok()
    }
}

fn file_sha256(path: &Path) -> Result<(String, u64)> {
    let mut file = File::open(path).with_context(|| format!("open {}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    let mut size = 0u64;
    loop {
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        size += n as u64;
    }
    Ok((hex::encode(hasher.finalize()), size))
}

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() || haystack.len() < needle.len() {
        return false;
    }
    haystack.windows(needle.len()).any(|w| w == needle)
}

pub fn inspect(path: &Path) -> Result<ApkInfo> {
    if !path.is_file() {
        bail!("APK not found: {}", path.display());
    }
    let (sha256, size) = file_sha256(path)?;

    let file = File::open(path)?;
    let mut zip = ZipArchive::new(file).with_context(|| "not a valid APK/ZIP archive")?;

    let mut manifest = Vec::new();
    zip.by_name("AndroidManifest.xml")
        .map_err(|_| anyhow!("AndroidManifest.xml is missing from the APK"))?
        .read_to_end(&mut manifest)?;
    let ManifestInfo {
        package,
        version_name,
        version_code,
    } = crate::axml::parse_manifest(&manifest)?;

    let names: Vec<String> = zip.file_names().filter_map(Result::ok).map(|c| c.to_string()).collect();
    let signed = names.iter().any(|n| {
        n.starts_with("META-INF/")
            && (n.ends_with(".RSA") || n.ends_with(".DSA") || n.ends_with(".EC"))
    });

    let needles: &[(&str, &[u8])] = &[
        ("Play licensing", b"com/android/vending/licensing"),
        ("LicenseVerification", b"LicenseVerification"),
        ("Google Play services", b"com/google/android/gms"),
        ("Billing", b"com/android/billingclient"),
        ("Firebase", b"com/google/firebase"),
    ];
    let mut license_hits = Vec::new();
    for i in 0..zip.len() {
        let mut entry = match zip.by_index(i) {
            Ok(e) => e,
            Err(_) => continue,
        };
        let name = entry.name().map(|c| c.to_string()).unwrap_or_default();
        if !(name.starts_with("classes") && name.ends_with(".dex")) {
            continue;
        }
        if entry.size() > 256 * 1024 * 1024 {
            continue;
        }
        let mut dex = Vec::with_capacity(entry.size() as usize);
        if entry.read_to_end(&mut dex).is_err() {
            continue;
        }
        for (label, needle) in needles {
            if contains(&dex, needle) && !license_hits.iter().any(|h| h == label) {
                license_hits.push((*label).to_string());
            }
        }
    }

    let patch = if sha256.eq_ignore_ascii_case(KNOWN_DEV_PATCHED_SHA256) {
        PatchStatus::KnownPatched
    } else if !signed {
        PatchStatus::LikelyPatched
    } else if !license_hits.is_empty() {
        PatchStatus::PossiblyStock
    } else if package.is_empty() || version_name.is_empty() {
        PatchStatus::Unknown
    } else {
        PatchStatus::LikelyPatched
    };

    Ok(ApkInfo {

        package,
        version_name,
        version_code,
        sha256,
        size,
        patch,
        license_hits,
        signed,
    })
}

pub fn inspect_nro(path: &Path) -> Result<(u64, Option<String>)> {
    let data = std::fs::read(path).with_context(|| format!("read {}", path.display()))?;
    if data.len() < 0x20 || &data[0x10..0x14] != b"NRO0" {
        bail!("not a Switch NRO (missing NRO0 header)");
    }
    let version = find_version(&data);
    Ok((data.len() as u64, version))
}

fn find_version(data: &[u8]) -> Option<String> {
    // The loader embeds VECTOR_VERSION as a plain string; show the first
    // plausible 0.x.y[-perfN] match.
    for i in 0..data.len().saturating_sub(8) {
        if data[i] == b'0' && data[i + 1] == b'.' && data[i + 3] == b'.' {
            let mut end = i;
            while end < data.len()
                && end - i < 32
                && (data[end].is_ascii_alphanumeric()
                    || data[end] == b'.'
                    || data[end] == b'-'
                    || data[end] == b'_')
            {
                end += 1;
            }
            if end - i >= 5 {
                let s = String::from_utf8_lossy(&data[i..end]).to_string();
                if s.chars().filter(|c| *c == '.').count() >= 2 {
                    return Some(s);
                }
            }
        }
    }
    None
}
