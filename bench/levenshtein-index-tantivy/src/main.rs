//! Independent Tantivy fuzzy-term baseline for immutable-dictionary retrieval.
//!
//! Each dictionary string is indexed as one untokenized term in one document. Transpositions are disabled for classic
//! Levenshtein semantics. `DocSetCollector` materializes all matching document addresses, but Tantivy does not return
//! edit distances, making this comparison deliberately favorable to Tantivy.

use std::env;
use std::fs;
use std::time::Instant;
use tantivy::collector::DocSetCollector;
use tantivy::query::FuzzyTermQuery;
use tantivy::schema::{STRING, Schema};
use tantivy::{Index, Term, doc};

fn load_lines(path: &str, limit: usize) -> Result<Vec<String>, Box<dyn std::error::Error>> {
    let contents = fs::read_to_string(path)?;
    Ok(contents
        .lines()
        .take(if limit == 0 { usize::MAX } else { limit })
        .map(|line| line.trim_end_matches('\r').to_owned())
        .collect())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 || args.len() > 4 {
        eprintln!("usage: levenshtein-index-tantivy DICTIONARY QUERIES [QUERY_LIMIT]");
        std::process::exit(2);
    }
    let query_limit = args.get(3).map_or(Ok(0), |value| value.parse::<usize>())?;
    let repeats = env::var("TANTIVY_REPEATS").map_or(Ok(3), |value| value.parse::<usize>())?;
    let dictionary = load_lines(&args[1], 0)?;
    let queries = load_lines(&args[2], query_limit)?;

    let mut schema_builder = Schema::builder();
    let word_field = schema_builder.add_text_field("word", STRING);
    let index = Index::create_in_ram(schema_builder.build());
    let build_start = Instant::now();
    let mut writer = index.writer(50_000_000)?;
    for word in &dictionary {
        writer.add_document(doc!(word_field => word.as_str()))?;
    }
    writer.commit()?;
    writer.wait_merging_threads()?;
    let reader = index.reader()?;
    let searcher = reader.searcher();
    println!(
        "dictionary={} queries={} build={:.6}s segments={} output=id-only",
        dictionary.len(),
        queries.len(),
        build_start.elapsed().as_secs_f64(),
        searcher.segment_readers().len()
    );

    for bound in 1u8..=4 {
        let mut failed = false;
        for repeat in 0..repeats {
            let start = Instant::now();
            let mut matches_count = 0usize;
            let mut checksum = 0u64;
            for query_text in &queries {
                let term = Term::from_field_text(word_field, query_text);
                let query = FuzzyTermQuery::new(term, bound, false);
                let matches = match searcher.search(&query, &DocSetCollector) {
                    Ok(matches) => matches,
                    Err(error) => {
                        eprintln!("k={bound} SKIPPED: {error}");
                        failed = true;
                        break;
                    }
                };
                matches_count += matches.len();
                for address in matches {
                    let id = (u64::from(address.segment_ord) << 32) | u64::from(address.doc_id);
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
