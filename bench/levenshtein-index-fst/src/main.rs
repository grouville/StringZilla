//! Independent `fst` Levenshtein-automaton baseline for immutable-dictionary retrieval.
//!
//! `fst` scores Unicode scalar values and returns matching keys/values, but not their distances. Run this baseline on
//! ASCII data when comparing it to StringZilla's byte index, or set `FST_ALLOW_UNICODE=1` when comparing it to the
//! separately named UTF-8/codepoint index. StringZilla materializes the richer `(dictionary ID, distance)` result, so
//! this benchmark is deliberately favorable to `fst`.

use fst::automaton::Levenshtein;
use fst::{IntoStreamer, Map, Streamer};
use std::env;
use std::fs;
use std::time::Instant;

fn load_lines(path: &str, limit: usize) -> Result<Vec<String>, Box<dyn std::error::Error>> {
    let contents = fs::read_to_string(path)?;
    Ok(contents
        .lines()
        .take(if limit == 0 { usize::MAX } else { limit })
        .map(|line| line.trim_end_matches('\r').to_owned())
        .collect())
}

fn verify_unicode_contract() -> Result<(), Box<dyn std::error::Error>> {
    // All one-codepoint strings are mutually within one substitution. In fst 0.4.7, substitutions between two
    // multibyte scalars sharing the same first UTF-8 byte are unexpectedly absent; fail before timing incomparable
    // output if that pinned behavior is still present.
    let mut keys = vec!["é", "Ѐ", "А"];
    keys.sort_unstable();
    let map = Map::from_iter(keys.iter().enumerate().map(|(id, key)| (*key, id as u64)))?;
    let mut matches = 0usize;
    for query in &keys {
        let automaton = Levenshtein::new(query, 1)?;
        let mut stream = map.search(&automaton).into_stream();
        while stream.next().is_some() {
            matches += 1;
        }
    }
    if matches != 9 {
        return Err(format!(
            "fst Unicode smoke test failed: expected 9 one-codepoint matches, observed {matches}"
        )
        .into());
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 || args.len() > 4 {
        eprintln!("usage: levenshtein-index-fst DICTIONARY QUERIES [QUERY_LIMIT]");
        std::process::exit(2);
    }
    let query_limit = args.get(3).map_or(Ok(0), |value| value.parse::<usize>())?;
    let dictionary = load_lines(&args[1], 0)?;
    let queries = load_lines(&args[2], query_limit)?;
    let state_limit = env::var("FST_STATE_LIMIT")
        .ok()
        .map(|value| value.parse::<usize>())
        .transpose()?;
    let repeats = env::var("FST_REPEATS").map_or(Ok(3), |value| value.parse::<usize>())?;
    let print_matches = env::var_os("FST_PRINT_MATCHES").is_some();
    let allow_unicode = env::var_os("FST_ALLOW_UNICODE").is_some();
    if allow_unicode {
        verify_unicode_contract()?;
    }
    if !allow_unicode && dictionary
        .iter()
        .chain(&queries)
        .any(|text| !text.is_ascii())
    {
        return Err(
            "the fst comparison harness requires ASCII inputs for byte-semantic parity".into(),
        );
    }

    let mut keyed_words: Vec<(&str, u64)> = dictionary
        .iter()
        .enumerate()
        .map(|(id, word)| (word.as_str(), id as u64))
        .collect();
    keyed_words.sort_unstable_by_key(|&(word, _)| word);
    if keyed_words.windows(2).any(|pair| pair[0].0 == pair[1].0) {
        return Err("fst::Map cannot preserve duplicate dictionary entries".into());
    }
    let build_start = Instant::now();
    let map = Map::from_iter(keyed_words)?;
    println!(
        "dictionary={} queries={} semantics={} build={:.6}s fst_bytes={}",
        dictionary.len(),
        queries.len(),
        if allow_unicode { "unicode-codepoints" } else { "ascii-byte-parity" },
        build_start.elapsed().as_secs_f64(),
        map.as_fst().as_bytes().len()
    );

    for bound in 1..=4 {
        let mut failed = false;
        for repeat in 0..repeats {
            let start = Instant::now();
            let mut matches_count = 0usize;
            let mut checksum = 0u64;
            for query in &queries {
                let automaton_result = match state_limit {
                    Some(limit) => Levenshtein::new_with_limit(query, bound, limit),
                    None => Levenshtein::new(query, bound),
                };
                let automaton = match automaton_result {
                    Ok(automaton) => automaton,
                    Err(error) => {
                        eprintln!("k={bound} SKIPPED: {error}");
                        failed = true;
                        break;
                    }
                };
                let mut stream = map.search(&automaton).into_stream();
                while let Some((key, id)) = stream.next() {
                    if print_matches {
                        eprintln!("k={bound} query={query:?} key={:?}", String::from_utf8_lossy(key));
                    }
                    matches_count += 1;
                    checksum = checksum
                        .wrapping_mul(0x9E37_79B1_85EB_CA87)
                        .wrapping_add(id);
                }
            }
            if failed {
                break;
            }
            println!(
                "k={bound} repeat={repeat} query={:.6}s matches={} checksum={checksum:016x} output=id-only",
                start.elapsed().as_secs_f64(),
                matches_count
            );
        }
    }
    Ok(())
}
