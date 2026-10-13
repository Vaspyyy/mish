//! Builtins operate exclusively on the shell's owned state and buffered output.
use crate::shell::{Outcome, Shell};
use std::path::Path;

pub const NAMES: &[&str] = &[
    "exit", "which", "echo", "cd", "pwd", "env", "export", "unset", "path", "alias", "unalias",
    "return", "hash",
];

pub fn run(shell: &mut Shell, argv: &[String]) -> Option<Outcome> {
    let name = argv.first()?.as_str();
    let args = &argv[1..];
    Some(match name {
        "exit" => exit(shell, args),
        "which" => which(shell, args),
        "echo" => {
            let mut out = Outcome::success();
            out.write(format!("{}\n", args.join(" ")));
            out
        }
        "cd" => cd(shell, args),
        "pwd" => {
            let mut out = Outcome::success();
            out.write(format!("{}\n", shell.cwd.display()));
            out
        }
        "env" => {
            let mut out = Outcome::success();
            for (name, value) in &shell.exported {
                out.write(format!("{name}=\x1b[1m{value}\x1b[0m\n"));
            }
            out
        }
        "export" => export(shell, args),
        "unset" => unset(shell, args),
        "path" => path(shell),
        "alias" => alias(shell, args),
        "unalias" => unalias(shell, args),
        "return" => match args.first() {
            None => Outcome::success(),
            Some(value) => match numeric_status(value) {
                Ok(status) => Outcome {
                    status,
                    ..Outcome::default()
                },
                Err(()) => Outcome::error(
                    2,
                    format!("return: numeric argument required: \"{value}\"."),
                ),
            },
        },
        "hash" => {
            let mut out = Outcome::success();
            for (name, path) in &shell.command_cache {
                out.write(format!("{name}=\x1b[1m{}\x1b[0m\n", path.display()));
            }
            if shell.command_cache.is_empty() {
                out.write("<no hashes defined>\n");
            }
            out
        }
        _ => return None,
    })
}

fn exit(shell: &Shell, args: &[String]) -> Outcome {
    if args.len() > 1 {
        return Outcome::error(2, "exit: too many arguments.");
    }
    let status = match args.first() {
        None => shell.last_status,
        Some(value) => match numeric_status(value) {
            Ok(status) => status,
            Err(()) => {
                return Outcome::error(2, format!("exit: numeric argument required: \"{value}\"."))
            }
        },
    }
    .rem_euclid(256);
    Outcome {
        status,
        exit: Some(status),
        ..Outcome::default()
    }
}

// C's strtol(..., 0) accepts signed decimal, octal, and hexadecimal values.
// Require the entire argument to be numeric instead of accepting silent junk.
fn numeric_status(value: &str) -> Result<i32, ()> {
    let value = value.trim();
    let (negative, digits) = match value.as_bytes().first() {
        Some(b'-') => (true, &value[1..]),
        Some(b'+') => (false, &value[1..]),
        _ => (false, value),
    };
    let (radix, digits) = if digits.starts_with("0x") || digits.starts_with("0X") {
        (16, &digits[2..])
    } else if digits.len() > 1 && digits.starts_with('0') {
        (8, &digits[1..])
    } else {
        (10, digits)
    };
    if digits.is_empty() || !digits.chars().all(|ch| ch.is_digit(radix)) {
        return Err(());
    }
    let magnitude = i64::from_str_radix(digits, radix).map_err(|_| ())?;
    let signed = if negative { -magnitude } else { magnitude };
    i32::try_from(signed).map_err(|_| ())
}

fn which(shell: &mut Shell, args: &[String]) -> Outcome {
    if args.is_empty() {
        return missing_arguments("which");
    }
    let mut only_path = false;
    let mut help = false;
    let mut operands = Vec::new();
    let mut flags = true;
    for arg in args {
        if flags && arg == "--" {
            flags = false;
        } else if flags && arg.starts_with('-') {
            match arg.as_str() {
                "--path-only" => only_path = true,
                "--help" => help = true,
                _ if !arg.starts_with("--") && arg.len() > 1 => {
                    for ch in arg[1..].chars() {
                        match ch {
                            'c' => only_path = true,
                            'h' => help = true,
                            _ => {
                                return Outcome::error(
                                    2,
                                    format!("which: unknown flag on \"{arg}\"."),
                                )
                            }
                        }
                    }
                }
                _ => return Outcome::error(2, format!("which: unknown flag on \"{arg}\".")),
            }
        } else {
            operands.push(arg);
        }
    }
    let mut out = Outcome::success();
    if help {
        out.write(concat!(
            "which built-in instructions:\nusage:\n\twhich -<flags> --<flags>\n",
            "\twhich will printf if the specified binary exists on the system, ",
            "indicating if it's a binary, built-in, or alias.\n\nflags:\n",
            "\t-h or --help: show this menu.\n",
            "\t-c or --path-only: only print the path of a binary.\n",
        ));
        return out;
    }
    if operands.is_empty() {
        return Outcome::error(1, "which: nothing was done!");
    }
    for name in operands {
        if let Some(replacement) = shell.aliases.get(name) {
            out.write(format!("\"{name}\" <aliased to> -> \"{replacement}\"\n"));
        } else if NAMES.contains(&name.as_str()) {
            out.write(format!("\"{name}\" -> <built-in>\n"));
        } else if let Some(resolved) = shell.resolve(name) {
            if only_path {
                out.write(format!("{}\n", resolved.display()));
            } else {
                out.write(format!("\"{name}\" -> \"{}\"\n", resolved.display()));
            }
        } else {
            out.status = 1;
            out.warn(format!("which: unknown binary \"{name}\"."));
        }
    }
    out
}

fn cd(shell: &mut Shell, args: &[String]) -> Outcome {
    let mut out = Outcome::success();
    if args.len() > 1 {
        out.warn("cd: excess arguments shall be ignored.");
    }
    let target = match args.first() {
        Some(value) => value.clone(),
        None => match shell.variable("HOME") {
            Some(value) => value,
            None => {
                out.status = 2;
                out.warn("cd: couldn't change to home directory: HOME is not set");
                return out;
            }
        },
    };
    let selected = Path::new(&target);
    let selected = if selected.is_absolute() {
        selected.to_path_buf()
    } else {
        shell.cwd.join(selected)
    };
    let result = if target.is_empty() {
        Err("No such file or directory".to_string())
    } else {
        // Looking up the final directory alone doesn't require search access
        // inside it. The trailing `.` asks the OS to check that access, just as
        // chdir would, without changing the process's working directory.
        std::fs::metadata(selected.join("."))
            .and_then(|_| selected.canonicalize())
            .map_err(|error| error.to_string())
    };
    match result {
        Ok(new_cwd) => {
            let old_cwd = shell.cwd.to_string_lossy().into_owned();
            let pwd = new_cwd.to_string_lossy().into_owned();
            shell.cwd = new_cwd;
            shell.export("OLDPWD", Some(&old_cwd));
            shell.export("PWD", Some(&pwd));
            shell.refresh_path();
        }
        Err(error) => {
            out.status = 2;
            if args.is_empty() {
                out.warn(format!("cd: couldn't change to home directory: {error}"));
            } else {
                out.warn(format!("cd: couldn't move to \"{target}\": {error}"));
            }
        }
    }
    out
}

fn valid_variable(name: &str) -> bool {
    let mut chars = name.chars();
    matches!(chars.next(), Some(ch) if ch == '_' || ch.is_ascii_alphabetic())
        && chars.all(|ch| ch == '_' || ch.is_ascii_alphanumeric())
}

fn export(shell: &mut Shell, args: &[String]) -> Outcome {
    if args.is_empty() {
        return missing_arguments("export");
    }
    let mut out = Outcome::success();
    for arg in args {
        let (name, value) = match arg.split_once('=') {
            Some((name, value)) => (name, Some(value)),
            None => (arg.as_str(), None),
        };
        if !valid_variable(name) || value.is_some_and(|value| value.contains('\0')) {
            out.status = 1;
            out.warn(format!("export: malformed request for \"{arg}\"."));
            continue;
        }
        shell.export(name, value);
        if name == "PATH" {
            shell.refresh_path();
        }
    }
    out
}

fn unset(shell: &mut Shell, args: &[String]) -> Outcome {
    if args.is_empty() {
        return missing_arguments("unset");
    }
    let mut out = Outcome::success();
    for arg in args {
        if !valid_variable(arg) {
            out.status = 1;
            out.warn(format!("unset: malformed request for \"{arg}\"."));
        } else {
            shell.unset(arg);
            if arg == "PATH" {
                shell.refresh_path();
            }
        }
    }
    out
}

fn path(shell: &Shell) -> Outcome {
    let mut out = Outcome::success();
    let entries = shell.path_entries();
    for path in &entries {
        let parent = path.parent().unwrap_or(Path::new(""));
        let name = path.file_name().unwrap_or_default().to_string_lossy();
        out.write(format!(
            "{}/\x1b[1m{name}\x1b[0m\n",
            parent.to_string_lossy().trim_end_matches('/')
        ));
    }
    if entries.is_empty() {
        out.write("<no paths have been defined>\n");
    }
    out
}

fn alias(shell: &mut Shell, args: &[String]) -> Outcome {
    let mut out = Outcome::success();
    if args.is_empty() {
        for (name, replacement) in &shell.aliases {
            out.write(format!("{name}=\"\x1b[1m{replacement}\x1b[0m\"\n"));
        }
        if shell.aliases.is_empty() {
            out.write("<no aliases set>\n");
        }
        return out;
    }
    for arg in args {
        match arg.split_once('=') {
            Some((name, replacement)) if !name.is_empty() && !arg.contains('\0') => {
                shell
                    .aliases
                    .insert(name.to_string(), replacement.to_string());
            }
            _ => {
                out.status = 1;
                out.warn(format!("alias: malformed request for \"{arg}\"."));
            }
        }
    }
    out
}

fn unalias(shell: &mut Shell, args: &[String]) -> Outcome {
    if args.is_empty() {
        return missing_arguments("unalias");
    }
    let mut out = Outcome::success();
    for arg in args {
        if shell.aliases.remove(arg).is_none() {
            out.status = 1;
            out.warn("unalias: failed to remove an alias.");
        }
    }
    out
}

fn missing_arguments(name: &str) -> Outcome {
    Outcome::error(2, format!("{name}: not 'nuff arguments."))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::os::unix::fs::{MetadataExt, PermissionsExt};
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn shell() -> Shell {
        let mut shell = Shell::new().unwrap();
        shell.variables.clear();
        shell.exported.clear();
        shell.aliases.clear();
        shell.command_cache.clear();
        shell.cwd = std::env::temp_dir().canonicalize().unwrap();
        shell.last_status = 0;
        shell
    }

    fn invoke(shell: &mut Shell, args: &[&str]) -> Outcome {
        run(
            shell,
            &args.iter().map(|arg| arg.to_string()).collect::<Vec<_>>(),
        )
        .unwrap()
    }

    struct Fixture(PathBuf);

    impl Fixture {
        fn new() -> Self {
            let nonce = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let path =
                std::env::temp_dir().join(format!("mish-builtins-{}-{nonce}", std::process::id()));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn aliases_replace_and_unalias_is_registered() {
        let mut shell = shell();
        assert_eq!(invoke(&mut shell, &["alias", "hi=echo old"]).status, 0);
        assert_eq!(
            invoke(&mut shell, &["alias", "hi=echo new=value"]).status,
            0
        );
        assert_eq!(
            invoke(&mut shell, &["which", "hi"]).stdout,
            b"\"hi\" <aliased to> -> \"echo new=value\"\n"
        );
        assert_eq!(shell.aliases.len(), 1);
        assert_eq!(invoke(&mut shell, &["unalias", "hi"]).status, 0);
        assert_eq!(invoke(&mut shell, &["unalias", "hi"]).status, 1);
        assert!(NAMES.contains(&"unalias"));
    }

    #[test]
    fn exporting_preserves_value_and_unsetting_removes_both_contexts() {
        let mut shell = shell();
        shell.assign("MISH_BUILTIN_TEST", "first=value".to_string());
        assert_eq!(
            invoke(&mut shell, &["export", "MISH_BUILTIN_TEST"]).status,
            0
        );
        assert_eq!(
            shell.exported.get("MISH_BUILTIN_TEST").unwrap(),
            "first=value"
        );
        assert_eq!(
            invoke(&mut shell, &["env"]).stdout,
            b"MISH_BUILTIN_TEST=\x1b[1mfirst=value\x1b[0m\n"
        );
        invoke(&mut shell, &["unset", "MISH_BUILTIN_TEST"]);
        assert!(shell.variable("MISH_BUILTIN_TEST").is_none());
        assert!(!shell.exported.contains_key("MISH_BUILTIN_TEST"));
    }

    #[test]
    fn failed_cd_preserves_cwd_and_directory_variables() {
        let mut shell = shell();
        shell.export("PWD", Some("kept-pwd"));
        shell.export("OLDPWD", Some("kept-oldpwd"));
        let cwd = shell.cwd.clone();
        let out = invoke(&mut shell, &["cd", "\0"]);
        assert_eq!(out.status, 2);
        assert_eq!(shell.cwd, cwd);
        assert_eq!(shell.variable("PWD").as_deref(), Some("kept-pwd"));
        assert_eq!(shell.variable("OLDPWD").as_deref(), Some("kept-oldpwd"));
    }

    #[test]
    fn cd_requires_search_access_and_preserves_state_on_permission_failure() {
        let fixture = Fixture::new();
        // A newly created fixture is owned by the test process. Root can bypass
        // these permissions, so the denial assertion applies to nonroot runs.
        if fs::metadata(&fixture.0).unwrap().uid() == 0 {
            return;
        }
        let denied = fixture.0.join("denied");
        fs::create_dir(&denied).unwrap();
        fs::set_permissions(&denied, fs::Permissions::from_mode(0o600)).unwrap();
        let mut shell = shell();
        shell.cwd = fixture.0.clone();
        shell.export("PWD", Some("kept-pwd"));
        shell.export("OLDPWD", Some("kept-oldpwd"));
        shell
            .command_cache
            .insert("kept".into(), PathBuf::from("/kept"));
        let process_cwd = std::env::current_dir().unwrap();
        let canonical = denied.canonicalize();
        let search = fs::metadata(denied.join("."));
        let out = invoke(&mut shell, &["cd", "denied"]);
        // Restore access before assertions so failed assertions still allow
        // the fixture's destructor to remove the temporary directory.
        fs::set_permissions(&denied, fs::Permissions::from_mode(0o700)).unwrap();
        assert!(
            canonical.is_ok(),
            "the previous validation permits this target"
        );
        assert_eq!(
            search.unwrap_err().kind(),
            std::io::ErrorKind::PermissionDenied
        );
        assert_eq!(out.status, 2);
        assert!(!out.stderr.is_empty());
        assert_eq!(shell.cwd, fixture.0);
        assert_eq!(shell.variable("PWD").as_deref(), Some("kept-pwd"));
        assert_eq!(shell.variable("OLDPWD").as_deref(), Some("kept-oldpwd"));
        assert!(shell.command_cache.contains_key("kept"));
        assert_eq!(std::env::current_dir().unwrap(), process_cwd);
    }

    #[test]
    fn cd_uses_owned_cwd_and_does_not_change_process_directory() {
        let mut shell = shell();
        let process_cwd = std::env::current_dir().unwrap();
        let old_cwd = shell.cwd.clone();
        assert_eq!(invoke(&mut shell, &["cd", "."]).status, 0);
        assert_eq!(std::env::current_dir().unwrap(), process_cwd);
        assert_eq!(
            shell.exported.get("OLDPWD").unwrap(),
            &old_cwd.to_string_lossy()
        );
        assert_eq!(
            shell.exported.get("PWD").unwrap(),
            &shell.cwd.to_string_lossy()
        );
    }

    #[test]
    fn status_commands_support_base_zero_and_report_invalid_arguments() {
        let mut shell = shell();
        assert_eq!(invoke(&mut shell, &["return", "010"]).status, 8);
        assert_eq!(invoke(&mut shell, &["return", "0x2a"]).status, 42);
        assert_eq!(invoke(&mut shell, &["return", "-7"]).status, -7);
        assert_eq!(invoke(&mut shell, &["return", "12junk"]).status, 2);
        assert_eq!(invoke(&mut shell, &["return", "--1"]).status, 2);
        assert_eq!(invoke(&mut shell, &["return", "0x-1"]).status, 2);
        shell.last_status = 7;
        assert_eq!(invoke(&mut shell, &["exit"]).exit, Some(7));
        assert_eq!(invoke(&mut shell, &["exit", "-1"]).exit, Some(255));
        assert_eq!(invoke(&mut shell, &["exit", "0x101"]).exit, Some(1));
        assert_eq!(invoke(&mut shell, &["exit", "1", "2"]).exit, None);
        assert_eq!(invoke(&mut shell, &["exit", "bad"]).status, 2);
    }

    #[test]
    fn which_flags_classification_and_partial_failure() {
        let mut shell = shell();
        assert_eq!(invoke(&mut shell, &["which", "-h"]).status, 0);
        assert_eq!(invoke(&mut shell, &["which", "--help"]).status, 0);
        assert_eq!(invoke(&mut shell, &["which", "-hc"]).status, 0);
        assert_eq!(invoke(&mut shell, &["which", "-z"]).status, 2);
        assert_eq!(invoke(&mut shell, &["which", "--path-only"]).status, 1);
        let out = invoke(&mut shell, &["which", "-c", "echo", "no-such-command"]);
        assert_eq!(out.status, 1);
        assert_eq!(out.stdout, b"\"echo\" -> <built-in>\n");
        assert_eq!(out.stderr, b"which: unknown binary \"no-such-command\".\n");
    }

    #[test]
    fn listing_empty_state_and_echo_preserve_output() {
        let mut shell = shell();
        assert_eq!(
            invoke(&mut shell, &["path"]).stdout,
            b"<no paths have been defined>\n"
        );
        assert_eq!(
            invoke(&mut shell, &["hash"]).stdout,
            b"<no hashes defined>\n"
        );
        assert_eq!(invoke(&mut shell, &["alias"]).stdout, b"<no aliases set>\n");
        assert_eq!(invoke(&mut shell, &["echo", "", "a  b"]).stdout, b" a  b\n");
        shell
            .command_cache
            .insert("tool".into(), PathBuf::from("/bin/tool"));
        assert_eq!(
            invoke(&mut shell, &["hash"]).stdout,
            b"tool=\x1b[1m/bin/tool\x1b[0m\n"
        );
    }

    #[test]
    fn executable_lookup_and_cd_use_relative_shell_paths() {
        let fixture = Fixture::new();
        let bin = fixture.0.join("bin");
        fs::create_dir(&bin).unwrap();
        let executable = bin.join("fixture-tool");
        fs::write(&executable, "#!/bin/sh\nexit 0\n").unwrap();
        fs::set_permissions(&executable, fs::Permissions::from_mode(0o700)).unwrap();
        fs::write(bin.join("plain-file"), "not executable").unwrap();
        fs::set_permissions(bin.join("plain-file"), fs::Permissions::from_mode(0o600)).unwrap();
        let mut shell = shell();
        shell.cwd = fixture.0.clone();
        shell.export("PATH", Some("bin"));
        for flag in ["-c", "--path-only"] {
            assert_eq!(
                invoke(&mut shell, &["which", flag, "fixture-tool"]).stdout,
                format!("{}\n", executable.display()).as_bytes()
            );
        }
        assert_eq!(
            invoke(&mut shell, &["path"]).stdout,
            format!("{}/\x1b[1mfixture-tool\x1b[0m\n", bin.display()).as_bytes()
        );
        assert!(shell.command_cache.contains_key("fixture-tool"));
        let out = invoke(&mut shell, &["cd", "bin"]);
        assert_eq!(out.status, 0);
        assert_eq!(shell.cwd, bin.canonicalize().unwrap());
        assert!(shell.command_cache.is_empty());
        assert_eq!(
            shell.exported.get("OLDPWD").unwrap(),
            &fixture.0.to_string_lossy()
        );
    }
}
