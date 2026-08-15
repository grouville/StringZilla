//! Official SymSpell-Rust indexed baseline, filtered back to exact Levenshtein semantics.
//!
//! SymSpell retrieves with optimal-string-alignment Damerau-Levenshtein distance, so adjacent transpositions can add
//! results outside StringZilla's contract. We request `Verbosity::All` and verify every returned candidate with plain
//! Levenshtein distance. The filter can only remove SymSpell results; the binary stream comparison detects omissions.

use std::collections::HashMap;
use std::env;
use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::time::Instant;
use symspell_rs::{SymSpell, Verbosity};

#[derive(Clone, Copy)]
struct Match {
    id: u32,
    distance: u8,
}

fn load_lines(path: &str, limit: usize) -> Result<Vec<String>, Box<dyn std::error::Error>> {
    let contents = fs::read_to_string(path)?;
    Ok(contents
        .lines()
        .take(if limit == 0 { usize::MAX } else { limit })
        .map(|line| line.trim_end_matches('\r').to_owned())
        .collect())
}

fn levenshtein_within(first: &str, second: &str, bound: usize) -> Option<usize> {
    let first: Vec<char> = first.chars().collect();
    let second: Vec<char> = second.chars().collect();
    if first.len().abs_diff(second.len()) > bound {
        return None;
    }
    let mut previous: Vec<usize> = (0..=first.len()).collect();
    let mut current = vec![0; first.len() + 1];
    for (row, &second_char) in second.iter().enumerate() {
        current[0] = row + 1;
        let mut row_min = current[0];
        for (column, &first_char) in first.iter().enumerate() {
            current[column + 1] = (previous[column + 1] + 1)
                .min(current[column] + 1)
                .min(previous[column] + usize::from(first_char != second_char));
            row_min = row_min.min(current[column + 1]);
        }
        if row_min > bound {
            return None;
        }
        std::mem::swap(&mut previous, &mut current);
    }
    (previous[first.len()] <= bound).then_some(previous[first.len()])
}

fn search(
    symspell: &SymSpell,
    ids: &HashMap<String, u32>,
    query: &str,
    bound: usize,
) -> Vec<Match> {
    let mut matches: Vec<Match> = symspell
        .lookup(query, Verbosity::All, bound, &None, None, false)
        .into_iter()
        .filter_map(|suggestion| {
            levenshtein_within(query, &suggestion.term, bound).map(|distance| Match {
                id: ids[&suggestion.term],
                distance: distance as u8,
            })
        })
        .collect();
    matches.sort_unstable_by_key(|found| (found.id, found.distance));
    matches
}

fn dump_matches(
    symspell: &SymSpell,
    ids: &HashMap<String, u32>,
    dictionary_size: usize,
    queries: &[String],
    bound: usize,
    path: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut output = BufWriter::new(File::create(path)?);
    output.write_all(b"SZLEV001")?;
    output.write_all(&(dictionary_size as u64).to_ne_bytes())?;
    output.write_all(&(queries.len() as u64).to_ne_bytes())?;
    output.write_all(&[bound as u8])?;
    for query in queries {
        let matches = search(symspell, ids, query, bound);
        output.write_all(&(matches.len() as u64).to_ne_bytes())?;
        for found in matches {
            output.write_all(&found.id.to_ne_bytes())?;
            output.write_all(&[found.distance])?;
        }
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 || args.len() > 5 {
        eprintln!(
            "usage: levenshtein-index-symspell DICTIONARY QUERIES [QUERY_LIMIT] [DUMP_PREFIX]"
        );
        std::process::exit(2);
    }
    let query_limit = args.get(3).map_or(Ok(0), |value| value.parse::<usize>())?;
    let dump_prefix = args.get(4).map(String::as_str);
    let dictionary = load_lines(&args[1], 0)?;
    let queries = load_lines(&args[2], query_limit)?;
    if dictionary
        .iter()
        .chain(&queries)
        .any(|text| text.to_lowercase() != *text)
    {
        return Err(
            "SymSpell comparison requires already-lowercase inputs for case-sensitive parity"
                .into(),
        );
    }
    let repeats = env::var("SYMSPELL_REPEATS").map_or(Ok(3), |value| value.parse::<usize>())?;
    if repeats == 0 {
        return Err("SYMSPELL_REPEATS must be positive".into());
    }
    let min_distance =
        env::var("SYMSPELL_MIN_DISTANCE").map_or(Ok(1), |value| value.parse::<usize>())?;
    let max_distance =
        env::var("SYMSPELL_MAX_DISTANCE").map_or(Ok(2), |value| value.parse::<usize>())?;
    if !(1..=max_distance).contains(&min_distance) || max_distance > 4 {
        return Err("SymSpell distance range must satisfy 1 <= minimum <= maximum <= 4".into());
    }
    let mut ids = HashMap::with_capacity(dictionary.len());
    for (id, word) in dictionary.iter().enumerate() {
        if ids.insert(word.to_lowercase(), id as u32).is_some() {
            return Err(
                "SymSpell cannot preserve duplicate or lowercase-colliding dictionary entries"
                    .into(),
            );
        }
    }
    println!(
        "dictionary={} queries={} semantics=utf8-codepoints+exact-levenshtein-filter",
        dictionary.len(),
        queries.len()
    );

    for bound in min_distance..=max_distance {
        let build_start = Instant::now();
        let mut symspell = SymSpell::new(bound, None, 7, 1);
        for word in &dictionary {
            symspell.create_dictionary_entry(word, 1);
        }
        println!(
            "k={bound} build={:.6}s indexed_words={}",
            build_start.elapsed().as_secs_f64(),
            symspell.get_dictionary_size()
        );
        for repeat in 0..repeats {
            let start = Instant::now();
            let mut matches_count = 0usize;
            let mut checksum = 0u64;
            for query in &queries {
                for found in search(&symspell, &ids, query, bound) {
                    matches_count += 1;
                    checksum = checksum
                        .wrapping_mul(0x9E37_79B1_85EB_CA87)
                        .wrapping_add(found.id as u64)
                        .wrapping_add(found.distance as u64);
                }
            }
            println!(
                "k={bound} repeat={repeat} query={:.6}s matches={matches_count} checksum={checksum:016x}",
                start.elapsed().as_secs_f64()
            );
        }
        if let Some(prefix) = dump_prefix {
            dump_matches(
                &symspell,
                &ids,
                dictionary.len(),
                &queries,
                bound,
                &format!("{prefix}.k{bound}.bin"),
            )?;
        }
    }
    Ok(())
}
