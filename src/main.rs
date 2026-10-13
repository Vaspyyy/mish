use mish::{
    parser,
    shell::{Outcome, Shell},
};
use std::fs::File;
use std::io::{self, IsTerminal, Read, Write};
use std::os::fd::AsFd;
use std::process::ExitCode;

const HELP: &str = "Usage: mish [-c COMMAND | SCRIPT]\n\nA small Unix shell. Without arguments, read commands from standard input.\n\n  -c COMMAND  execute one command\n  --help      show this help\n  --version   show the version\n\nVariables: $(NAME). Command substitution: $[command].\n";

fn main() -> ExitCode {
    match run() {
        Ok(status) => ExitCode::from(status.rem_euclid(256) as u8),
        Err(error) => {
            eprintln!("mish: {error}");
            ExitCode::from(1)
        }
    }
}

fn run() -> io::Result<i32> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut shell = Shell::new()?;
    match args.as_slice() {
        [flag] if flag == "--help" || flag == "-h" => {
            print!("{HELP}");
            Ok(0)
        }
        [flag] if flag == "--version" => {
            println!("mish {}", env!("CARGO_PKG_VERSION"));
            Ok(0)
        }
        [flag, command] if flag == "-c" => {
            let outcome = shell.run_line(command, false);
            emit(&outcome)?;
            Ok(outcome.exit.unwrap_or(outcome.status))
        }
        [script] if !script.starts_with('-') => {
            let mut input = File::open(script)?;
            run_input(&mut shell, &mut input, false)
        }
        [] => {
            let interactive = io::stdin().is_terminal();
            // Rust's Stdin buffers internally. Duplicate the descriptor so
            // shell reads cannot consume input intended for a foreground child.
            let mut input = File::from(io::stdin().as_fd().try_clone_to_owned()?);
            run_input(&mut shell, &mut input, interactive)
        }
        _ => {
            eprintln!("{HELP}");
            Ok(2)
        }
    }
}

fn emit(outcome: &Outcome) -> io::Result<()> {
    io::stdout().write_all(&outcome.stdout)?;
    io::stdout().flush()?;
    io::stderr().write_all(&outcome.stderr)?;
    io::stderr().flush()
}

fn run_input(shell: &mut Shell, input: &mut impl Read, interactive: bool) -> io::Result<i32> {
    loop {
        if interactive {
            let prompt = match shell.variable("PS1") {
                Some(prompt) => parser::expand_prompt(&prompt, shell).unwrap_or_else(|error| {
                    eprintln!("mish: prompt: {error}");
                    "> ".to_owned()
                }),
                None => "> ".to_owned(),
            };
            io::stderr().write_all(&shell.take_stderr())?;
            io::stderr().flush()?;
            print!("{prompt}");
            io::stdout().flush()?;
        }
        let Some(line) = read_line(input)? else {
            if interactive {
                println!();
            }
            return Ok(shell.last_status);
        };
        let outcome = shell.run_line(&line, false);
        emit(&outcome)?;
        if let Some(status) = outcome.exit {
            return Ok(status);
        }
    }
}

// Read without buffering ahead, so foreground commands can consume shell stdin.
fn read_line(input: &mut impl Read) -> io::Result<Option<String>> {
    let mut line = Vec::new();
    let mut byte = [0];
    loop {
        match input.read(&mut byte) {
            Ok(0) if line.is_empty() => return Ok(None),
            Ok(0) => break,
            Ok(_) if byte[0] == b'\n' => break,
            Ok(_) => line.push(byte[0]),
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error),
        }
    }
    String::from_utf8(line)
        .map(Some)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))
}
