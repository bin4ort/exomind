// Rust runner shim: read fn<TAB>args lines, print results.
use std::io::{self, BufRead};
fn main() {
    for line in io::stdin().lock().lines() {
        let line = line.unwrap();
        match line.split_once('\t') {
            Some((f, args)) => {
                // dispatch: match f { "add" => { let v:
                // Vec<i32> = args.split_whitespace()... } }
                println!("error: no such fn {f}");
            }
            None => {}
        }
    }
}
