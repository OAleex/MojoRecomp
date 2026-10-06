use std::collections::{HashMap, HashSet};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

const HEADER_SIZE: usize = 60;
const MAGIC: &[u8] = b"ATG CORE CEMENT LIBRARY";
const SUPPORTED_FLAGS: [u8; 4] = [2, 1, 1, 1];
const TABLE1_RECORD_SIZE: u64 = 12;
const TABLE2_PREFIX_SIZE: usize = 8;
const TABLE2_RECORD_HEADER_SIZE: usize = 16;
const TABLE2_RECORD_PADDING_SIZE: usize = 3;
const MAX_FILE_COUNT: u32 = 1_000_000;
const COPY_BUFFER_SIZE: usize = 1024 * 1024;

#[derive(Clone, Debug)]
pub struct Replacement {
    pub archive_path: String,
    pub source_path: PathBuf,
}

#[derive(Clone, Debug)]
pub struct RcfEntry {
    pub id: u32,
    pub offset: u64,
    pub size: u64,
    pub table1_index: usize,
    pub name: String,
}

#[derive(Clone, Debug)]
pub struct RcfIndex {
    pub entries: Vec<RcfEntry>,
    pub alignment: u64,
    pub data_start: u64,
    pub source_size: u64,
    table1_offset: u64,
    table2_offset: u64,
    table2_size: u64,
}

#[derive(Clone, Debug)]
struct PlannedEntry {
    source_index: usize,
    new_offset: u64,
    new_size: u64,
    replacement: Option<PathBuf>,
}

fn read_be_u32(bytes: &[u8]) -> u32 {
    u32::from_be_bytes(bytes.try_into().expect("u32 slice"))
}

fn read_le_u32(bytes: &[u8]) -> u32 {
    u32::from_le_bytes(bytes.try_into().expect("u32 slice"))
}

fn align_up(value: u64, alignment: u64) -> Result<u64, String> {
    if alignment == 0 || !alignment.is_power_of_two() {
        return Err("RCF alignment must be a non-zero power of two".into());
    }
    value
        .checked_add(alignment - 1)
        .map(|rounded| rounded & !(alignment - 1))
        .ok_or_else(|| "RCF alignment overflow".to_string())
}

fn normalize_archive_path(value: &str) -> Result<String, String> {
    if value.is_empty() {
        return Err("RCF entry path is empty".into());
    }
    let normalized = value.replace('/', "\\");
    if normalized.starts_with('\\')
        || normalized.contains(':')
        || normalized
            .split('\\')
            .any(|part| part.is_empty() || part == "." || part == "..")
    {
        return Err(format!("Unsafe RCF entry path: {value}"));
    }
    Ok(normalized.to_ascii_lowercase())
}

fn validate_range(offset: u64, size: u64, limit: u64, label: &str) -> Result<(), String> {
    let end = offset
        .checked_add(size)
        .ok_or_else(|| format!("{label} range overflows"))?;
    if end > limit {
        return Err(format!("{label} range exceeds the archive"));
    }
    Ok(())
}

pub fn inspect(path: &Path) -> Result<RcfIndex, String> {
    let mut file = File::open(path)
        .map_err(|error| format!("Could not open RCF {}: {error}", path.display()))?;
    inspect_file(&mut file)
}

pub fn read_entry(path: &Path, archive_path: &str, max_size: u64) -> Result<Vec<u8>, String> {
    let expected = normalize_archive_path(archive_path)?;
    let mut file = File::open(path)
        .map_err(|error| format!("Could not open RCF {}: {error}", path.display()))?;
    let index = inspect_file(&mut file)?;
    let entry = index
        .entries
        .iter()
        .find(|entry| {
            normalize_archive_path(&entry.name)
                .map(|name| name == expected)
                .unwrap_or(false)
        })
        .ok_or_else(|| format!("RCF entry is missing: {archive_path}"))?;
    if entry.size > max_size {
        return Err(format!(
            "RCF entry is too large to patch safely: {} bytes",
            entry.size
        ));
    }
    let size: usize = entry
        .size
        .try_into()
        .map_err(|_| "RCF entry is too large for this host".to_string())?;
    let mut bytes = vec![0u8; size];
    file.seek(SeekFrom::Start(entry.offset))
        .and_then(|_| file.read_exact(&mut bytes))
        .map_err(|error| format!("Could not read RCF entry {archive_path}: {error}"))?;
    Ok(bytes)
}

fn inspect_file(file: &mut File) -> Result<RcfIndex, String> {
    let source_size = file
        .metadata()
        .map_err(|error| format!("Could not query RCF size: {error}"))?
        .len();
    if source_size < HEADER_SIZE as u64 {
        return Err("RCF is smaller than its fixed header".into());
    }

    file.seek(SeekFrom::Start(0))
        .map_err(|error| format!("Could not seek RCF header: {error}"))?;
    let mut header = [0u8; HEADER_SIZE];
    file.read_exact(&mut header)
        .map_err(|error| format!("Could not read RCF header: {error}"))?;

    if &header[..MAGIC.len()] != MAGIC || header[MAGIC.len()..32].iter().any(|byte| *byte != 0) {
        return Err("Unsupported RCF signature".into());
    }
    if header[32..36] != SUPPORTED_FLAGS {
        return Err(format!(
            "Unsupported RCF flag combination: {:02x?}",
            &header[32..36]
        ));
    }

    let table1_offset = read_be_u32(&header[36..40]) as u64;
    let table1_size = read_be_u32(&header[40..44]) as u64;
    let table2_offset = read_be_u32(&header[44..48]) as u64;
    let table2_size = read_be_u32(&header[48..52]) as u64;
    let reserved = read_be_u32(&header[52..56]);
    let file_count = read_be_u32(&header[56..60]);

    if reserved != 0 {
        return Err("Unsupported non-zero RCF header reserved field".into());
    }
    if file_count == 0 || file_count > MAX_FILE_COUNT {
        return Err(format!("Invalid RCF file count: {file_count}"));
    }
    let expected_table1_size = (file_count as u64)
        .checked_mul(TABLE1_RECORD_SIZE)
        .ok_or_else(|| "RCF table 1 size overflow".to_string())?;
    if table1_size != expected_table1_size {
        return Err(format!(
            "RCF table 1 size {table1_size} does not match {file_count} entries"
        ));
    }
    validate_range(table1_offset, table1_size, source_size, "RCF table 1")?;
    validate_range(table2_offset, table2_size, source_size, "RCF table 2")?;
    if table1_offset < HEADER_SIZE as u64
        || table2_offset < table1_offset + table1_size
        || table2_size < TABLE2_PREFIX_SIZE as u64
    {
        return Err("RCF index tables overlap or are out of order".into());
    }

    let table1_len: usize = table1_size
        .try_into()
        .map_err(|_| "RCF table 1 is too large for this host".to_string())?;
    let mut table1 = vec![0u8; table1_len];
    file.seek(SeekFrom::Start(table1_offset))
        .and_then(|_| file.read_exact(&mut table1))
        .map_err(|error| format!("Could not read RCF table 1: {error}"))?;

    let mut entries = Vec::with_capacity(file_count as usize);
    for index in 0..file_count as usize {
        let base = index * TABLE1_RECORD_SIZE as usize;
        let id = read_be_u32(&table1[base..base + 4]);
        let offset = read_be_u32(&table1[base + 4..base + 8]) as u64;
        let size = read_be_u32(&table1[base + 8..base + 12]) as u64;
        entries.push(RcfEntry {
            id,
            offset,
            size,
            table1_index: index,
            name: String::new(),
        });
    }

    let table2_len: usize = table2_size
        .try_into()
        .map_err(|_| "RCF table 2 is too large for this host".to_string())?;
    let mut table2 = vec![0u8; table2_len];
    file.seek(SeekFrom::Start(table2_offset))
        .and_then(|_| file.read_exact(&mut table2))
        .map_err(|error| format!("Could not read RCF table 2: {error}"))?;

    let alignment = read_le_u32(&table2[0..4]) as u64;
    let table2_reserved = read_le_u32(&table2[4..8]);
    if alignment != 2048 || table2_reserved != 0 {
        return Err(format!(
            "Unsupported RCF table 2 header: alignment={alignment}, reserved={table2_reserved}"
        ));
    }

    let mut physical_order: Vec<usize> = (0..entries.len()).collect();
    physical_order.sort_unstable_by_key(|index| entries[*index].offset);
    let data_start = align_up(
        table2_offset
            .checked_add(table2_size)
            .ok_or_else(|| "RCF data-start overflow".to_string())?,
        alignment,
    )?;

    let mut previous_end = data_start;
    let mut seen_offsets = HashSet::with_capacity(entries.len());
    for &entry_index in &physical_order {
        let entry = &entries[entry_index];
        if entry.offset < data_start || entry.offset % alignment != 0 {
            return Err(format!(
                "RCF entry {} has invalid aligned offset {}",
                entry.table1_index, entry.offset
            ));
        }
        if !seen_offsets.insert(entry.offset) {
            return Err(format!(
                "RCF contains duplicate data offset {}",
                entry.offset
            ));
        }
        if entry.offset < previous_end {
            return Err("RCF data entries overlap".into());
        }
        validate_range(entry.offset, entry.size, source_size, "RCF data entry")?;
        previous_end = entry
            .offset
            .checked_add(entry.size)
            .ok_or_else(|| "RCF data-entry end overflow".to_string())?;
    }
    if entries[physical_order[0]].offset != data_start {
        return Err(format!(
            "RCF first data entry starts at {}, expected {data_start}",
            entries[physical_order[0]].offset
        ));
    }
    if previous_end != source_size {
        return Err(format!(
            "RCF final data entry ends at {previous_end}, archive size is {source_size}"
        ));
    }

    let mut cursor = TABLE2_PREFIX_SIZE;
    let mut seen_names = HashSet::with_capacity(entries.len());
    for &entry_index in &physical_order {
        let header_end = cursor
            .checked_add(TABLE2_RECORD_HEADER_SIZE)
            .ok_or_else(|| "RCF table 2 cursor overflow".to_string())?;
        if header_end > table2.len() {
            return Err("RCF table 2 ended before all entry records".into());
        }

        let name_len = read_le_u32(&table2[cursor + 12..cursor + 16]) as usize;
        if name_len < 2 {
            return Err("RCF table 2 contains an invalid empty name".into());
        }
        let name_start = header_end;
        let name_end = name_start
            .checked_add(name_len)
            .ok_or_else(|| "RCF table 2 name length overflow".to_string())?;
        let record_end = name_end
            .checked_add(TABLE2_RECORD_PADDING_SIZE)
            .ok_or_else(|| "RCF table 2 record overflow".to_string())?;
        if record_end > table2.len() {
            return Err("RCF table 2 name exceeds its declared table size".into());
        }
        let raw_name = &table2[name_start..name_end];
        if raw_name.last() != Some(&0) || raw_name[..raw_name.len() - 1].contains(&0) {
            return Err("RCF table 2 name is not a single NUL-terminated string".into());
        }
        if table2[name_end..record_end].iter().any(|byte| *byte != 0) {
            return Err("RCF table 2 entry padding is not zero".into());
        }
        let name = std::str::from_utf8(&raw_name[..raw_name.len() - 1])
            .map_err(|_| "RCF table 2 entry name is not ASCII/UTF-8".to_string())?;
        if !name.is_ascii() {
            return Err("RCF table 2 entry name is not ASCII".into());
        }
        let normalized = normalize_archive_path(name)?;
        if !seen_names.insert(normalized) {
            return Err(format!("RCF contains duplicate entry name: {name}"));
        }
        entries[entry_index].name = name.to_string();
        cursor = record_end;
    }
    if cursor != table2.len() {
        return Err(format!(
            "RCF table 2 has {} trailing bytes after the final entry",
            table2.len() - cursor
        ));
    }

    Ok(RcfIndex {
        entries,
        alignment,
        data_start,
        source_size,
        table1_offset,
        table2_offset,
        table2_size,
    })
}

fn replacement_map(
    index: &RcfIndex,
    replacements: &[Replacement],
) -> Result<HashMap<usize, PathBuf>, String> {
    let mut entry_by_name = HashMap::with_capacity(index.entries.len());
    for (entry_index, entry) in index.entries.iter().enumerate() {
        entry_by_name.insert(normalize_archive_path(&entry.name)?, entry_index);
    }

    let mut mapped = HashMap::with_capacity(replacements.len());
    let mut seen_requested = HashSet::with_capacity(replacements.len());
    for replacement in replacements {
        let normalized = normalize_archive_path(&replacement.archive_path)?;
        if !seen_requested.insert(normalized.clone()) {
            return Err(format!(
                "Duplicate RCF replacement target: {}",
                replacement.archive_path
            ));
        }
        let Some(&entry_index) = entry_by_name.get(&normalized) else {
            return Err(format!(
                "RCF replacement target is missing from the archive: {}",
                replacement.archive_path
            ));
        };
        if !replacement.source_path.is_file() {
            return Err(format!(
                "RCF replacement source does not exist: {}",
                replacement.source_path.display()
            ));
        }
        mapped.insert(entry_index, replacement.source_path.clone());
    }
    Ok(mapped)
}

fn plan_rebuild(
    index: &RcfIndex,
    replacements: &HashMap<usize, PathBuf>,
) -> Result<(Vec<PlannedEntry>, u64), String> {
    let mut physical_order: Vec<usize> = (0..index.entries.len()).collect();
    physical_order.sort_unstable_by_key(|entry_index| index.entries[*entry_index].offset);

    let mut next_offset = index.data_start;
    let mut plan = Vec::with_capacity(index.entries.len());
    for entry_index in physical_order {
        let entry = &index.entries[entry_index];
        let replacement = replacements.get(&entry_index).cloned();
        let new_size = match &replacement {
            Some(path) => fs::metadata(path)
                .map_err(|error| {
                    format!("Could not query replacement {}: {error}", path.display())
                })?
                .len(),
            None => entry.size,
        };
        if new_size > u32::MAX as u64 {
            return Err(format!(
                "RCF entry {} exceeds the 32-bit size field",
                entry.name
            ));
        }
        next_offset = align_up(next_offset, index.alignment)?;
        if next_offset > u32::MAX as u64 {
            return Err("RCF data offset exceeds the 32-bit format limit".into());
        }
        plan.push(PlannedEntry {
            source_index: entry_index,
            new_offset: next_offset,
            new_size,
            replacement,
        });
        next_offset = next_offset
            .checked_add(new_size)
            .ok_or_else(|| "RCF rebuilt size overflow".to_string())?;
    }
    if next_offset > u32::MAX as u64 {
        return Err("Rebuilt RCF exceeds the 32-bit archive limit".into());
    }
    Ok((plan, next_offset))
}

struct CopyProgress<'a, F> {
    buffer: &'a mut [u8],
    copied: &'a mut u64,
    total: u64,
    on_progress: &'a mut F,
}

fn copy_exact_range<F>(
    source: &mut File,
    output: &mut File,
    source_offset: u64,
    size: u64,
    progress: &mut CopyProgress<'_, F>,
) -> Result<(), String>
where
    F: FnMut(u64, u64) -> Result<(), String>,
{
    source
        .seek(SeekFrom::Start(source_offset))
        .map_err(|error| format!("Could not seek RCF source data: {error}"))?;
    let mut remaining = size;
    while remaining != 0 {
        let chunk = usize::try_from(remaining.min(progress.buffer.len() as u64))
            .map_err(|_| "RCF copy chunk is too large".to_string())?;
        source
            .read_exact(&mut progress.buffer[..chunk])
            .map_err(|error| format!("Could not read RCF source data: {error}"))?;
        output
            .write_all(&progress.buffer[..chunk])
            .map_err(|error| format!("Could not write rebuilt RCF data: {error}"))?;
        remaining -= chunk as u64;
        *progress.copied = progress
            .copied
            .checked_add(chunk as u64)
            .ok_or_else(|| "RCF progress counter overflow".to_string())?;
        (progress.on_progress)(*progress.copied, progress.total)?;
    }
    Ok(())
}

fn write_zero_padding(output: &mut File, count: u64, buffer: &mut [u8]) -> Result<(), String> {
    buffer.fill(0);
    let mut remaining = count;
    while remaining != 0 {
        let chunk = usize::try_from(remaining.min(buffer.len() as u64))
            .map_err(|_| "RCF padding chunk is too large".to_string())?;
        output
            .write_all(&buffer[..chunk])
            .map_err(|error| format!("Could not write RCF alignment padding: {error}"))?;
        remaining -= chunk as u64;
    }
    Ok(())
}

pub fn rebuild_to_path(
    source_path: &Path,
    output_path: &Path,
    replacements: &[Replacement],
    mut on_progress: impl FnMut(u64, u64) -> Result<(), String>,
) -> Result<RcfIndex, String> {
    if replacements.is_empty() {
        return Err("RCF rebuild requires at least one replacement".into());
    }
    if source_path == output_path {
        return Err("RCF rebuild output must not overwrite the source archive".into());
    }

    let index = inspect(source_path)?;
    let replacements = replacement_map(&index, replacements)?;
    let (plan, final_size) = plan_rebuild(&index, &replacements)?;

    if let Some(parent) = output_path.parent() {
        fs::create_dir_all(parent).map_err(|error| {
            format!(
                "Could not create rebuilt RCF output directory {}: {error}",
                parent.display()
            )
        })?;
    }
    let mut source =
        File::open(source_path).map_err(|error| format!("Could not reopen source RCF: {error}"))?;
    let mut output = OpenOptions::new()
        .create(true)
        .truncate(true)
        .read(true)
        .write(true)
        .open(output_path)
        .map_err(|error| format!("Could not create rebuilt RCF: {error}"))?;

    let total_copy_bytes = plan
        .iter()
        .try_fold(0u64, |total, planned| total.checked_add(planned.new_size))
        .ok_or_else(|| "RCF progress total overflow".to_string())?;

    let mut buffer = vec![0u8; COPY_BUFFER_SIZE];
    let mut copied = 0u64;
    let progress_total = index
        .data_start
        .checked_add(total_copy_bytes)
        .ok_or_else(|| "RCF progress total overflow".to_string())?;
    copy_exact_range(
        &mut source,
        &mut output,
        0,
        index.data_start,
        &mut CopyProgress {
            buffer: &mut buffer,
            copied: &mut copied,
            total: progress_total,
            on_progress: &mut on_progress,
        },
    )?;

    let mut new_table1 = vec![0u8; index.entries.len() * TABLE1_RECORD_SIZE as usize];
    for planned in &plan {
        let entry = &index.entries[planned.source_index];
        let base = entry.table1_index * TABLE1_RECORD_SIZE as usize;
        new_table1[base..base + 4].copy_from_slice(&entry.id.to_be_bytes());
        new_table1[base + 4..base + 8].copy_from_slice(&(planned.new_offset as u32).to_be_bytes());
        new_table1[base + 8..base + 12].copy_from_slice(&(planned.new_size as u32).to_be_bytes());
    }
    output
        .seek(SeekFrom::Start(index.table1_offset))
        .and_then(|_| output.write_all(&new_table1))
        .map_err(|error| format!("Could not update rebuilt RCF table 1: {error}"))?;
    output
        .seek(SeekFrom::Start(index.data_start))
        .map_err(|error| format!("Could not seek rebuilt RCF data area: {error}"))?;

    for planned in &plan {
        let current = output
            .stream_position()
            .map_err(|error| format!("Could not query rebuilt RCF position: {error}"))?;
        if current > planned.new_offset {
            return Err("RCF rebuild plan produced overlapping output entries".into());
        }
        write_zero_padding(&mut output, planned.new_offset - current, &mut buffer)?;

        if let Some(replacement) = &planned.replacement {
            let mut replacement_file = File::open(replacement).map_err(|error| {
                format!(
                    "Could not open RCF replacement {}: {error}",
                    replacement.display()
                )
            })?;
            copy_exact_range(
                &mut replacement_file,
                &mut output,
                0,
                planned.new_size,
                &mut CopyProgress {
                    buffer: &mut buffer,
                    copied: &mut copied,
                    total: progress_total,
                    on_progress: &mut on_progress,
                },
            )?;
        } else {
            let entry = &index.entries[planned.source_index];
            copy_exact_range(
                &mut source,
                &mut output,
                entry.offset,
                entry.size,
                &mut CopyProgress {
                    buffer: &mut buffer,
                    copied: &mut copied,
                    total: progress_total,
                    on_progress: &mut on_progress,
                },
            )?;
        }
    }
    output
        .set_len(final_size)
        .map_err(|error| format!("Could not finalize rebuilt RCF size: {error}"))?;
    output
        .sync_all()
        .map_err(|error| format!("Could not flush rebuilt RCF: {error}"))?;
    drop(output);

    let rebuilt = inspect(output_path)?;
    if rebuilt.entries.len() != index.entries.len()
        || rebuilt.alignment != index.alignment
        || rebuilt.data_start != index.data_start
        || rebuilt.table2_offset != index.table2_offset
        || rebuilt.table2_size != index.table2_size
    {
        return Err("Rebuilt RCF index does not match the source archive structure".into());
    }
    for (original, new) in index.entries.iter().zip(&rebuilt.entries) {
        if original.id != new.id || original.name != new.name {
            return Err("Rebuilt RCF changed an entry ID or name".into());
        }
    }

    Ok(rebuilt)
}

pub fn estimate_rebuild_size(
    source_path: &Path,
    replacements: &[Replacement],
) -> Result<u64, String> {
    let index = inspect(source_path)?;
    let replacements = replacement_map(&index, replacements)?;
    let (_, final_size) = plan_rebuild(&index, &replacements)?;
    Ok(final_size)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_root(name: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        std::env::temp_dir().join(format!(
            "mojorecomp-rcf-{name}-{}-{nonce}",
            std::process::id()
        ))
    }

    fn append_table2_record(table: &mut Vec<u8>, name: &str) {
        let name_len = name.len() + 1;
        table.extend_from_slice(&0x1234_5678u32.to_le_bytes());
        table.extend_from_slice(&2048u32.to_le_bytes());
        table.extend_from_slice(&0u32.to_le_bytes());
        table.extend_from_slice(&(name_len as u32).to_le_bytes());
        table.extend_from_slice(name.as_bytes());
        table.push(0);
        table.extend_from_slice(&[0, 0, 0]);
    }

    fn create_synthetic_archive(path: &Path) -> Vec<(String, Vec<u8>)> {
        let files = vec![
            ("package\\first.p3d".to_string(), b"P3D\xFFfirst".to_vec()),
            (
                "package\\second.p3d".to_string(),
                b"P3D\xFFsecond-data".to_vec(),
            ),
            ("script\\boot.lua".to_string(), b"print('ok')".to_vec()),
        ];
        let file_count = files.len() as u32;
        let table1_offset = HEADER_SIZE as u32;
        let table1_size = file_count * TABLE1_RECORD_SIZE as u32;
        let table2_offset = 2048u32;
        let mut table2 = Vec::new();
        table2.extend_from_slice(&2048u32.to_le_bytes());
        table2.extend_from_slice(&0u32.to_le_bytes());
        for (name, _) in &files {
            append_table2_record(&mut table2, name);
        }
        let table2_size = table2.len() as u32;
        let data_start = align_up(table2_offset as u64 + table2_size as u64, 2048).expect("align");

        let mut offsets = Vec::new();
        let mut next = data_start;
        for (_, bytes) in &files {
            next = align_up(next, 2048).expect("align");
            offsets.push(next);
            next += bytes.len() as u64;
        }

        let mut output = File::create(path).expect("create synthetic RCF");
        let mut header = [0u8; HEADER_SIZE];
        header[..MAGIC.len()].copy_from_slice(MAGIC);
        header[32..36].copy_from_slice(&SUPPORTED_FLAGS);
        header[36..40].copy_from_slice(&table1_offset.to_be_bytes());
        header[40..44].copy_from_slice(&table1_size.to_be_bytes());
        header[44..48].copy_from_slice(&table2_offset.to_be_bytes());
        header[48..52].copy_from_slice(&table2_size.to_be_bytes());
        header[56..60].copy_from_slice(&file_count.to_be_bytes());
        output.write_all(&header).expect("header");
        for (index, ((_, bytes), offset)) in files.iter().zip(&offsets).enumerate() {
            output
                .write_all(&(0xA000_0000u32 + index as u32).to_be_bytes())
                .expect("id");
            output
                .write_all(&(*offset as u32).to_be_bytes())
                .expect("offset");
            output
                .write_all(&(bytes.len() as u32).to_be_bytes())
                .expect("size");
        }
        let current = output.stream_position().expect("position");
        write_zero_padding(
            &mut output,
            table2_offset as u64 - current,
            &mut vec![0u8; 4096],
        )
        .expect("index padding");
        output.write_all(&table2).expect("table2");
        let current = output.stream_position().expect("position");
        write_zero_padding(&mut output, data_start - current, &mut vec![0u8; 4096])
            .expect("data padding");
        for ((_, bytes), offset) in files.iter().zip(&offsets) {
            let current = output.stream_position().expect("position");
            write_zero_padding(&mut output, *offset - current, &mut vec![0u8; 4096])
                .expect("entry padding");
            output.write_all(bytes).expect("entry");
        }
        files
    }

    #[test]
    fn parses_verified_cot_rcf_variant_and_maps_names_by_physical_order() {
        let root = test_root("parse");
        fs::create_dir_all(&root).expect("root");
        let archive = root.join("default.rcf");
        let files = create_synthetic_archive(&archive);

        let index = inspect(&archive).expect("inspect");
        assert_eq!(index.entries.len(), files.len());
        assert_eq!(index.alignment, 2048);
        let mut physical: Vec<&RcfEntry> = index.entries.iter().collect();
        physical.sort_unstable_by_key(|entry| entry.offset);
        for (entry, (name, bytes)) in physical.iter().zip(&files) {
            assert_eq!(&entry.name, name);
            assert_eq!(entry.size, bytes.len() as u64);
        }

        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn rebuild_recalculates_offsets_for_larger_and_smaller_replacements() {
        let root = test_root("rebuild");
        fs::create_dir_all(&root).expect("root");
        let archive = root.join("default.rcf");
        let output = root.join("localized.rcf");
        let files = create_synthetic_archive(&archive);

        let larger = root.join("larger.p3d");
        let smaller = root.join("smaller.p3d");
        fs::write(&larger, b"P3D\xFFreplacement-that-is-much-larger").expect("larger");
        fs::write(&smaller, b"P3D\xFFx").expect("smaller");
        let replacements = vec![
            Replacement {
                archive_path: files[0].0.clone(),
                source_path: larger.clone(),
            },
            Replacement {
                archive_path: files[1].0.clone(),
                source_path: smaller.clone(),
            },
        ];

        let rebuilt =
            rebuild_to_path(&archive, &output, &replacements, |_, _| Ok(())).expect("rebuild");
        let mut physical: Vec<&RcfEntry> = rebuilt.entries.iter().collect();
        physical.sort_unstable_by_key(|entry| entry.offset);
        assert_eq!(physical[0].size, fs::metadata(&larger).unwrap().len());
        assert_eq!(physical[1].size, fs::metadata(&smaller).unwrap().len());
        assert_eq!(physical[2].size, files[2].1.len() as u64);
        assert_eq!(physical[1].offset % 2048, 0);
        assert_eq!(physical[2].offset % 2048, 0);

        let mut rebuilt_file = File::open(&output).expect("open rebuilt");
        rebuilt_file
            .seek(SeekFrom::Start(physical[2].offset))
            .expect("seek unchanged");
        let mut unchanged = vec![0u8; physical[2].size as usize];
        rebuilt_file
            .read_exact(&mut unchanged)
            .expect("read unchanged");
        assert_eq!(unchanged, files[2].1);

        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn rebuild_rejects_missing_or_duplicate_replacements_without_touching_source() {
        let root = test_root("invalid");
        fs::create_dir_all(&root).expect("root");
        let archive = root.join("default.rcf");
        let files = create_synthetic_archive(&archive);
        let original = fs::read(&archive).expect("source bytes");
        let invalid = root.join("missing-replacement.bin");

        let result = rebuild_to_path(
            &archive,
            &root.join("invalid-output.rcf"),
            &[Replacement {
                archive_path: files[0].0.clone(),
                source_path: invalid,
            }],
            |_, _| Ok(()),
        );
        assert!(result.is_err());
        assert_eq!(fs::read(&archive).expect("source after failure"), original);

        let valid = root.join("valid.p3d");
        fs::write(&valid, b"P3D\xFFvalid").expect("valid replacement");
        assert!(
            rebuild_to_path(
                &archive,
                &root.join("missing-output.rcf"),
                &[Replacement {
                    archive_path: "package\\missing.p3d".into(),
                    source_path: valid.clone(),
                }],
                |_, _| Ok(())
            )
            .is_err()
        );
        assert_eq!(
            fs::read(&archive).expect("source after missing target"),
            original
        );

        let duplicate = vec![
            Replacement {
                archive_path: files[0].0.clone(),
                source_path: valid.clone(),
            },
            Replacement {
                archive_path: files[0].0.clone(),
                source_path: valid,
            },
        ];
        assert!(
            rebuild_to_path(
                &archive,
                &root.join("duplicate-output.rcf"),
                &duplicate,
                |_, _| Ok(())
            )
            .is_err()
        );
        assert_eq!(fs::read(&archive).expect("source final"), original);

        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn parser_fails_closed_on_corrupt_signature_and_truncated_table() {
        let root = test_root("corrupt");
        fs::create_dir_all(&root).expect("root");
        let archive = root.join("default.rcf");
        create_synthetic_archive(&archive);

        let mut bytes = fs::read(&archive).expect("read");
        bytes[0] ^= 0xFF;
        let corrupt = root.join("bad-signature.rcf");
        fs::write(&corrupt, &bytes).expect("write corrupt");
        assert!(inspect(&corrupt).is_err());

        let truncated = root.join("truncated.rcf");
        fs::write(&truncated, &bytes[..HEADER_SIZE + 10]).expect("write truncated");
        assert!(inspect(&truncated).is_err());

        create_synthetic_archive(&archive);
        let mut misaligned_bytes = fs::read(&archive).expect("read aligned archive");
        let first_offset_base = HEADER_SIZE + 4;
        let first_offset = read_be_u32(&misaligned_bytes[first_offset_base..first_offset_base + 4]);
        misaligned_bytes[first_offset_base..first_offset_base + 4]
            .copy_from_slice(&(first_offset + 1).to_be_bytes());
        let misaligned = root.join("misaligned.rcf");
        fs::write(&misaligned, misaligned_bytes).expect("write misaligned");
        assert!(inspect(&misaligned).is_err());

        fs::remove_dir_all(root).expect("cleanup");
    }
}
