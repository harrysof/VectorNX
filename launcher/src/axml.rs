//! Minimal Android binary XML parser.
//!
//! An APK's AndroidManifest.xml is not text; it is the `ResXMLTree` binary
//! format. We only need the `<manifest>` element's package/version attributes.

use anyhow::{anyhow, bail, Result};

const RES_STRING_POOL_TYPE: u16 = 0x0001;
const RES_XML_TYPE: u16 = 0x0003;
const RES_XML_START_ELEMENT_TYPE: u16 = 0x0102;

const UTF8_FLAG: u32 = 1 << 8;
const NO_INDEX: u32 = 0xFFFF_FFFF;
const TYPE_STRING: u8 = 0x03;
const TYPE_INT_DEC: u8 = 0x10;
const TYPE_INT_HEX: u8 = 0x11;

#[derive(Debug, Clone, Default)]
pub struct ManifestInfo {
    pub package: String,
    pub version_name: String,
    pub version_code: i64,
}

fn u16_at(data: &[u8], off: usize) -> Option<u16> {
    data.get(off..off + 2)
        .map(|b| u16::from_le_bytes([b[0], b[1]]))
}

fn u32_at(data: &[u8], off: usize) -> Option<u32> {
    data.get(off..off + 4)
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn parse_string_pool(data: &[u8], start: usize, header_size: usize) -> Result<Vec<String>> {
    let count = u32_at(data, start + 8).ok_or_else(|| anyhow!("string pool count"))? as usize;
    if count > 1_000_000 {
        bail!("string pool too large");
    }
    let flags = u32_at(data, start + 16).ok_or_else(|| anyhow!("string pool flags"))?;
    let strings_start = u32_at(data, start + 20).ok_or_else(|| anyhow!("string pool base"))? as usize;
    let offsets_at = start + header_size;
    let mut strings = Vec::with_capacity(count);
    for i in 0..count {
        let off = u32_at(data, offsets_at + i * 4)
            .ok_or_else(|| anyhow!("string offset {}", i))? as usize;
        let mut p = start + strings_start + off;
        if p >= data.len() {
            bail!("string out of range");
        }
        if flags & UTF8_FLAG != 0 {
            // u16 UTF-16 length, then a 1/2-byte UTF-8 byte length.
            p += 2;
            let b0 = *data.get(p).ok_or_else(|| anyhow!("string length"))? as usize;
            p += 1;
            let len = if b0 & 0x80 != 0 {
                let b1 = *data.get(p).ok_or_else(|| anyhow!("string length hi"))? as usize;
                p += 1;
                ((b0 & 0x7F) << 8) | b1
            } else {
                b0
            };
            let bytes = data
                .get(p..p + len)
                .ok_or_else(|| anyhow!("string bytes"))?;
            strings.push(String::from_utf8_lossy(bytes).into_owned());
        } else {
            let mut len = u16_at(data, p).ok_or_else(|| anyhow!("utf16 length"))? as usize;
            p += 2;
            if len & 0x8000 != 0 {
                let lo = u16_at(data, p).ok_or_else(|| anyhow!("utf16 length hi"))? as usize;
                p += 2;
                len = ((len & 0x7FFF) << 16) | lo;
            }
            let mut units = Vec::with_capacity(len);
            for _ in 0..len {
                units.push(u16_at(data, p).ok_or_else(|| anyhow!("utf16 unit"))?);
                p += 2;
            }
            strings.push(String::from_utf16_lossy(&units));
        }
    }
    Ok(strings)
}

fn attr_text(
    strings: &[String],
    raw: u32,
    data_type: u8,
    data_value: u32,
) -> Option<String> {
    if raw != NO_INDEX {
        return strings.get(raw as usize).cloned();
    }
    if data_type == TYPE_STRING {
        return strings.get(data_value as usize).cloned();
    }
    None
}

fn attr_int(
    strings: &[String],
    raw: u32,
    data_type: u8,
    data_value: u32,
) -> Option<i64> {
    match data_type {
        TYPE_INT_DEC | TYPE_INT_HEX => Some(data_value as i64),
        TYPE_STRING | _ => attr_text(strings, raw, data_type, data_value)
            .and_then(|s| s.trim().parse::<i64>().ok()),
    }
}

pub fn parse_manifest(data: &[u8]) -> Result<ManifestInfo> {
    if u16_at(data, 0) != Some(RES_XML_TYPE) {
        bail!("not an Android binary XML document");
    }
    let total = u32_at(data, 4).ok_or_else(|| anyhow!("xml size"))? as usize;
    let total = total.min(data.len());
    let mut strings: Vec<String> = Vec::new();
    let mut off = 8usize; // ResXMLTree header
    while off + 8 <= total {
        let kind = u16_at(data, off).unwrap_or(0);
        let header_size = u16_at(data, off + 2).unwrap_or(0) as usize;
        let size = u32_at(data, off + 4).unwrap_or(0) as usize;
        if size < 8 || off + size > data.len() || header_size < 8 || header_size > size {
            break;
        }
        match kind {
            RES_STRING_POOL_TYPE => {
                strings = parse_string_pool(data, off, header_size)?;
            }
            RES_XML_START_ELEMENT_TYPE => {
                // ResXMLTree_node (16) + ResXMLTree_attrExt (20)
                if size >= 36 {
                    let name_idx = u32_at(data, off + 20).unwrap_or(NO_INDEX) as usize;
                    let name = strings.get(name_idx).cloned().unwrap_or_default();
                    if name == "manifest" {
                        let attr_start = u16_at(data, off + 24).unwrap_or(0) as usize;
                        let attr_size = u16_at(data, off + 26).unwrap_or(20) as usize;
                        let attr_count = u16_at(data, off + 28).unwrap_or(0) as usize;
                        if attr_size < 20 || attr_count > 4096 {
                            bail!("invalid manifest attributes");
                        }
                        let base = off + 16 + attr_start;
                        let mut info = ManifestInfo::default();
                        for i in 0..attr_count {
                            let a = base + i * attr_size;
                            if a + 20 > data.len() {
                                break;
                            }
                            let raw = u32_at(data, a + 8).unwrap_or(NO_INDEX);
                            let data_type = *data.get(a + 15).unwrap_or(&0);
                            let data_value = u32_at(data, a + 16).unwrap_or(0);
                            let aname = u32_at(data, a + 4)
                                .and_then(|idx| strings.get(idx as usize))
                                .cloned()
                                .unwrap_or_default();
                            match aname.as_str() {
                                "package" => {
                                    info.package = attr_text(&strings, raw, data_type, data_value)
                                        .unwrap_or_default();
                                }
                                "versionName" => {
                                    info.version_name = attr_text(&strings, raw, data_type, data_value)
                                        .unwrap_or_default();
                                }
                                "versionCode" => {
                                    info.version_code =
                                        attr_int(&strings, raw, data_type, data_value).unwrap_or(0);
                                }
                                _ => {}
                            }
                        }
                        if info.package.is_empty() {
                            bail!("manifest has no package attribute");
                        }
                        return Ok(info);
                    }
                }
            }
            _ => {}
        }
        off += size;
    }
    bail!("manifest element not found")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_text_xml() {
        assert!(parse_manifest(b"<manifest package=\"x\"/>").is_err());
    }
}
