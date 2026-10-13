//! Owned shell state and Unix process execution.
use crate::{builtins, parser};
use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus};

#[derive(Clone)]
pub struct Shell {
    pub variables: BTreeMap<String, String>,
    pub exported: BTreeMap<String, String>,
    pub aliases: BTreeMap<String, String>,
    pub cwd: PathBuf,
    pub last_status: i32,
    pub command_cache: BTreeMap<String, PathBuf>,
    substitution_depth: usize,
    pending_stderr: Vec<u8>,
    expansion_error: Option<String>,
}

#[derive(Default, Debug)]
pub struct Outcome {
    pub status: i32,
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
    pub exit: Option<i32>,
}

impl Outcome {
    pub fn success() -> Self {
        Self::default()
    }

    pub fn error(status: i32, message: impl AsRef<str>) -> Self {
        let mut outcome = Self {
            status,
            ..Self::default()
        };
        outcome.warn(message);
        outcome
    }

    pub fn write(&mut self, text: impl AsRef<str>) {
        self.stdout.extend_from_slice(text.as_ref().as_bytes());
    }

    pub fn warn(&mut self, text: impl AsRef<str>) {
        self.stderr.extend_from_slice(text.as_ref().as_bytes());
        self.stderr.push(b'\n');
    }
}

impl Shell {
    pub fn new() -> io::Result<Self> {
        Ok(Self {
            variables: BTreeMap::new(),
            exported: std::env::vars_os()
                .filter_map(|(key, value)| {
                    Some((key.into_string().ok()?, value.into_string().ok()?))
                })
                .collect(),
            aliases: BTreeMap::new(),
            cwd: std::env::current_dir()?,
            last_status: 0,
            command_cache: BTreeMap::new(),
            substitution_depth: 0,
            pending_stderr: Vec::new(),
            expansion_error: None,
        })
    }

    pub fn variable(&self, name: &str) -> Option<String> {
        if name == "$" {
            return Some(self.last_status.to_string());
        }
        self.variables
            .get(name)
            .or_else(|| self.exported.get(name))
            .cloned()
    }

    pub fn assign(&mut self, name: &str, value: String) {
        if self.exported.contains_key(name) {
            self.exported.insert(name.to_owned(), value.clone());
        }
        self.variables.insert(name.to_owned(), value);
        if name == "PATH" {
            self.refresh_path();
        }
    }

    pub fn export(&mut self, name: &str, value: Option<&str>) {
        let value = value
            .map(str::to_owned)
            .or_else(|| self.variable(name))
            .unwrap_or_default();
        self.variables.insert(name.to_owned(), value.clone());
        self.exported.insert(name.to_owned(), value);
        if name == "PATH" {
            self.refresh_path();
        }
    }

    pub fn unset(&mut self, name: &str) {
        self.variables.remove(name);
        self.exported.remove(name);
        if name == "PATH" {
            self.refresh_path();
        }
    }

    pub fn refresh_path(&mut self) {
        self.command_cache.clear();
    }

    pub fn take_stderr(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.pending_stderr)
    }

    fn path_directories(&self) -> Vec<PathBuf> {
        self.variable("PATH").map_or_else(Vec::new, |path| {
            path.split(':').map(|dir| self.cwd.join(dir)).collect()
        })
    }

    pub fn path_entries(&self) -> Vec<PathBuf> {
        let mut entries = Vec::new();
        for directory in self.path_directories() {
            if let Ok(files) = fs::read_dir(directory) {
                let mut paths: Vec<_> = files
                    .filter_map(Result::ok)
                    .map(|file| file.path())
                    .filter(|path| executable(path))
                    .collect();
                paths.sort();
                entries.extend(paths);
            }
        }
        entries
    }

    pub fn resolve(&mut self, name: &str) -> Option<PathBuf> {
        if name.contains('/') {
            let path = self.cwd.join(name);
            return executable(&path).then_some(path);
        }
        if let Some(path) = self.command_cache.get(name) {
            if executable(path) {
                return Some(path.clone());
            }
            self.command_cache.remove(name);
        }
        for directory in self.path_directories() {
            let path = directory.join(name);
            if executable(&path) {
                self.command_cache.insert(name.to_owned(), path.clone());
                return Some(path);
            }
        }
        None
    }

    fn expand_alias(&self, input: &str) -> String {
        let start = input.len() - input.trim_start().len();
        let end = input[start..]
            .find(char::is_whitespace)
            .map_or(input.len(), |i| start + i);
        if let Some(replacement) = self.aliases.get(&input[start..end]) {
            format!("{}{}{}", &input[..start], replacement, &input[end..])
        } else {
            input.to_owned()
        }
    }

    pub fn run_line(&mut self, input: &str, capture: bool) -> Outcome {
        self.pending_stderr.clear();
        self.expansion_error = None;
        let input = self.expand_alias(input);
        let mut outcome = match parser::parse_line(&input, self) {
            Err(message) => {
                self.expansion_error = Some(message.clone());
                Outcome::error(2, format!("mish: {message}"))
            }
            Ok(argv) if argv.is_empty() => Outcome {
                status: if input.trim().is_empty() {
                    self.last_status
                } else {
                    0
                },
                ..Outcome::default()
            },
            Ok(argv) => builtins::run(self, &argv).unwrap_or_else(|| self.external(&argv, capture)),
        };
        let mut stderr = std::mem::take(&mut self.pending_stderr);
        stderr.append(&mut outcome.stderr);
        outcome.stderr = stderr;
        self.last_status = outcome.status;
        outcome
    }

    fn external(&mut self, argv: &[String], capture: bool) -> Outcome {
        let Some(path) = self.resolve(&argv[0]) else {
            // Distinguish an existing but non-executable explicit path.
            let status = if argv[0].contains('/') && self.cwd.join(&argv[0]).exists() {
                126
            } else {
                127
            };
            return Outcome::error(
                status,
                format!(
                    "mish: couldn't execute \"{}\": {}",
                    argv[0],
                    if status == 126 {
                        "Permission denied"
                    } else {
                        "Unknown binary"
                    }
                ),
            );
        };
        let mut command = Command::new(path);
        command
            .args(&argv[1..])
            .current_dir(&self.cwd)
            .env_clear()
            .envs(&self.exported);
        if capture {
            match command.output() {
                Ok(output) => Outcome {
                    status: status_code(output.status),
                    stdout: output.stdout,
                    stderr: output.stderr,
                    exit: None,
                },
                Err(error) => spawn_error(&argv[0], error),
            }
        } else {
            match command.status() {
                Ok(status) => Outcome {
                    status: status_code(status),
                    ..Outcome::default()
                },
                Err(error) => spawn_error(&argv[0], error),
            }
        }
    }
}

fn executable(path: &Path) -> bool {
    fs::metadata(path)
        .is_ok_and(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
}

fn status_code(status: ExitStatus) -> i32 {
    status
        .code()
        .unwrap_or_else(|| 128 + status.signal().unwrap_or(0))
}

fn spawn_error(name: &str, error: io::Error) -> Outcome {
    let status = if error.kind() == io::ErrorKind::NotFound {
        127
    } else {
        126
    };
    Outcome::error(
        status,
        format!("mish: couldn't execute \"{name}\": {error}"),
    )
}

impl parser::ExpansionContext for Shell {
    fn variable(&self, name: &str) -> Option<String> {
        self.variable(name)
    }

    fn assign(&mut self, name: &str, value: String) {
        self.assign(name, value);
    }

    fn command(&mut self, source: &str) -> Result<String, String> {
        if self.substitution_depth >= 32 {
            return Err("command substitution nested too deeply".to_owned());
        }
        let mut child = self.clone();
        child.substitution_depth += 1;
        child.pending_stderr.clear();
        let outcome = child.run_line(source, true);
        if let Some(error) = child.expansion_error {
            return Err(error);
        }
        self.pending_stderr.extend(outcome.stderr);
        Ok(String::from_utf8_lossy(&outcome.stdout)
            .trim_end_matches('\n')
            .to_owned())
    }
}
