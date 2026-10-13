#![cfg(unix)]

use std::fs;
use std::io::{Read, Write};
use std::os::unix::fs::{symlink, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant};

static NEXT_DIR: AtomicU64 = AtomicU64::new(0);

struct Sandbox {
    root: PathBuf,
}

impl Sandbox {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!(
            "mish-test-{}-{}",
            std::process::id(),
            NEXT_DIR.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&root).unwrap();
        Self { root }
    }

    fn path(&self, name: &str) -> PathBuf {
        self.root.join(name)
    }

    fn executable(&self, name: &str, body: &str) -> PathBuf {
        let path = self.path(name);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, format!("#!/bin/sh\n{body}\n")).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        path
    }

    fn command(&self) -> Command {
        let mut command = Command::new(env!("CARGO_BIN_EXE_mish"));
        command
            .env_clear()
            .env("HOME", &self.root)
            .env("PATH", self.path("bin"))
            .env("LC_ALL", "C")
            .env("PS1", "PROMPT-MUST-NOT-APPEAR> ")
            .current_dir(&self.root);
        command
    }

    fn stdin(&self, source: &str) -> Output {
        run(self.command(), source)
    }

    fn line(&self, source: &str) -> Output {
        let mut command = self.command();
        command.args(["-c", source]);
        run(command, "")
    }
}

impl Drop for Sandbox {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

// Drain both pipes while waiting. Every CLI test fails within a bounded time,
// including regressions that wait for EOF or fill a command-substitution pipe.
fn run(mut command: Command, source: &str) -> Output {
    let mut child = command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let mut stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let out = thread::spawn(move || {
        let mut bytes = Vec::new();
        stdout.read_to_end(&mut bytes).unwrap();
        bytes
    });
    let err = thread::spawn(move || {
        let mut bytes = Vec::new();
        stderr.read_to_end(&mut bytes).unwrap();
        bytes
    });
    child
        .stdin
        .take()
        .unwrap()
        .write_all(source.as_bytes())
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    let status = loop {
        if let Some(status) = child.try_wait().unwrap() {
            break status;
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            panic!("mish exceeded the 10-second CLI test timeout");
        }
        thread::sleep(Duration::from_millis(10));
    };
    Output {
        status,
        stdout: out.join().unwrap(),
        stderr: err.join().unwrap(),
    }
}

fn stdout(output: &Output) -> &str {
    std::str::from_utf8(&output.stdout).unwrap()
}

fn plain_stdout(output: &Output) -> String {
    stdout(output).replace("\x1b[1m", "").replace("\x1b[0m", "")
}

fn success(output: &Output) {
    assert!(output.status.success(), "{output:?}");
    assert!(output.stderr.is_empty(), "{output:?}");
}

fn quote(path: &Path) -> String {
    let path = path.to_str().unwrap();
    assert!(!path.contains('\''));
    format!("'{path}'")
}

#[test]
fn command_script_and_stdin_modes() {
    let sandbox = Sandbox::new();
    let output = sandbox.line("echo command mode");
    success(&output);
    assert_eq!(stdout(&output), "command mode\n");

    let script = sandbox.path("script with spaces.mish");
    fs::write(&script, "echo first\necho second\necho final").unwrap();
    let mut command = sandbox.command();
    command.arg(script);
    let output = run(command, "echo ignored\n");
    success(&output);
    assert_eq!(stdout(&output), "first\nsecond\nfinal\n");

    let output = sandbox.stdin("\n \t\necho one\n\necho two");
    success(&output);
    assert_eq!(stdout(&output), "one\ntwo\n");
    let output = sandbox.stdin("");
    success(&output);
    assert!(output.stdout.is_empty());
}

#[test]
fn help_version_and_cli_errors() {
    let sandbox = Sandbox::new();
    for option in ["--help", "--version"] {
        let mut command = sandbox.command();
        command.arg(option);
        let output = run(command, "");
        success(&output);
        assert!(stdout(&output).contains("mish"));
    }
    for args in [vec!["-c"], vec!["--unknown"], vec!["missing-script.mish"]] {
        let mut command = sandbox.command();
        command.args(args);
        let output = run(command, "");
        assert!(!output.status.success());
        assert!(!output.stderr.is_empty());
    }
}

#[test]
fn quotes_escapes_empty_arguments_and_unicode() {
    let sandbox = Sandbox::new();
    sandbox.executable("bin/args", "for arg do printf '<%s>\\n' \"$arg\"; done");
    let output = sandbox.line(
        "args '' \"\" 'two words' escaped\\ space 'Grüße 世界 🦀' '$HOME' '$[echo no]' '\\literal'",
    );
    success(&output);
    assert_eq!(stdout(&output), "<>\n<>\n<two words>\n<escaped space>\n<Grüße 世界 🦀>\n<$HOME>\n<$[echo no]>\n<\\literal>\n");
}

#[test]
fn assignments_export_unset_and_child_inheritance() {
    let sandbox = Sandbox::new();
    sandbox.executable(
        "bin/showenv",
        "printf '<%s>|<%s>\\n' \"${LOCAL-unset}\" \"${SECOND-unset}\"",
    );
    let output = sandbox.stdin(
        "LOCAL='first value' SECOND=$(LOCAL) echo $(SECOND)\nshowenv\nexport LOCAL\nshowenv\nLOCAL=changed\nshowenv\nexport SECOND=two\nshowenv\nunset LOCAL SECOND\nshowenv\necho '<'$(LOCAL)'>'\n",
    );
    success(&output);
    assert_eq!(stdout(&output), "first value\n<unset>|<unset>\n<first value>|<unset>\n<changed>|<unset>\n<changed>|<two>\n<unset>|<unset>\n<>\n");
    let output = sandbox.stdin("export SAMPLE='plain value'\nenv\n");
    success(&output);
    assert!(plain_stdout(&output)
        .lines()
        .any(|line| line == "SAMPLE=plain value"));
}

#[test]
fn cwd_home_oldpwd_and_failed_cd() {
    let sandbox = Sandbox::new();
    fs::create_dir(sandbox.path("child dir")).unwrap();
    let output = sandbox.stdin("pwd\ncd 'child dir'\npwd\necho $(PWD)\necho $(OLDPWD)\ncd\npwd\n");
    success(&output);
    let root = sandbox.root.display();
    let child = sandbox.path("child dir");
    assert_eq!(
        stdout(&output),
        format!(
            "{root}\n{}\n{}\n{root}\n{root}\n",
            child.display(),
            child.display()
        )
    );
    let output = sandbox.stdin("cd missing\npwd\n");
    assert!(output.status.success());
    assert!(!output.stderr.is_empty());
    assert_eq!(stdout(&output), format!("{root}\n"));
}

#[test]
fn path_order_symlinks_nonexecutables_and_refresh() {
    let sandbox = Sandbox::new();
    let first = sandbox.executable("first/tool", "printf 'first\\n'");
    let second = sandbox.executable("second/tool", "printf 'second\\n'");
    symlink(&first, sandbox.path("first/linked")).unwrap();
    fs::write(sandbox.path("first/nonexec"), "not executable").unwrap();
    let first_dir = quote(&sandbox.path("first"));
    let second_dir = quote(&sandbox.path("second"));
    let output = sandbox.stdin(&format!(
        "PATH={first_dir}:{second_dir}\ntool\nlinked\nwhich --path-only tool\npath\nhash\nexport PATH={second_dir}:{first_dir}\ntool\nwhich -c tool\n"
    ));
    success(&output);
    let text = plain_stdout(&output);
    assert!(text.starts_with(&format!("first\nfirst\n{}\n", first.display())));
    assert!(text.contains(&sandbox.path("first/linked").display().to_string()));
    assert!(!text.contains("nonexec"));
    assert!(text.ends_with(&format!("second\n{}\n", second.display())));
}

#[test]
fn relative_and_empty_path_entries_follow_cd() {
    let sandbox = Sandbox::new();
    sandbox.executable("here", "printf 'root\\n'");
    sandbox.executable("next/here", "printf 'next\\n'");
    sandbox.executable("next/bin/relative", "printf 'relative\\n'");
    let output = sandbox.stdin("PATH=\nhere\ncd next\nhere\nPATH=bin\nrelative\n./here\n");
    success(&output);
    assert_eq!(stdout(&output), "root\nnext\nrelative\nnext\n");
}

#[test]
fn aliases_replace_once_and_unalias_is_registered() {
    let sandbox = Sandbox::new();
    let output = sandbox.stdin(
        "alias greet='echo hello'\ngreet world\nalias greet='echo updated'\ngreet\nalias\nwhich greet echo unalias\nunalias greet\nalias\n",
    );
    success(&output);
    let text = stdout(&output);
    assert!(text.starts_with("hello world\nupdated\n"));
    assert!(text.contains("greet="));
    assert!(text.contains("unalias") && text.contains("built-in"));
    assert_eq!(text.matches("greet=").count(), 1);
    let output = sandbox.stdin("alias a=b\nalias b='echo recursive'\na\n");
    assert!(!output.status.success());
    assert!(!stdout(&output).contains("recursive"));
}

#[test]
fn nested_capture_whitespace_and_isolated_state() {
    let sandbox = Sandbox::new();
    fs::create_dir(sandbox.path("child")).unwrap();
    sandbox.executable("bin/whitespace", "printf '  left\\nright  \\n\\n'");
    let output = sandbox.stdin(
        "VALUE=outside\necho $[echo $[echo nested]]\necho '<'$[whitespace]'>'\necho $[VALUE=inside echo $(VALUE)]\necho $(VALUE)\necho $[cd child]\npwd\necho $[alias hidden='echo leaked']\nwhich hidden\necho survived\n",
    );
    assert!(output.status.success());
    assert!(!output.stderr.is_empty()); // The captured alias must not leak.
    assert_eq!(
        stdout(&output),
        format!(
            "nested\n<  left\nright  >\ninside\noutside\n\n{}\n\nsurvived\n",
            sandbox.root.display()
        )
    );
}

#[test]
fn substitutions_drain_output_larger_than_pipe_capacity() {
    let sandbox = Sandbox::new();
    // Use a file, avoiding OS argv size limits and external utility options.
    let payload = "x".repeat(512 * 1024);
    fs::write(sandbox.path("payload"), &payload).unwrap();
    sandbox.executable("bin/large", "exec /bin/cat payload");
    let output = sandbox.line("echo $[large]");
    success(&output);
    assert_eq!(output.stdout.len(), payload.len() + 1);
    assert_eq!(&output.stdout[..payload.len()], payload.as_bytes());
    assert_eq!(output.stdout.last(), Some(&b'\n'));
}

#[test]
fn return_exit_and_external_status_are_actual_exit_codes() {
    let sandbox = Sandbox::new();
    sandbox.executable("bin/fail", "exit 23");
    let output = sandbox.line("fail");
    assert_eq!(output.status.code(), Some(23));
    let output = sandbox.stdin("fail\necho $($)\nreturn 17\necho $($)\nexit 7\necho unreachable\n");
    assert_eq!(output.status.code(), Some(7));
    assert_eq!(stdout(&output), "23\n17\n");
    assert!(output.stderr.is_empty());
    let output = sandbox.stdin("return 9\n\n \t\n");
    assert_eq!(output.status.code(), Some(9));
    let output = sandbox.line("return 11");
    assert_eq!(output.status.code(), Some(11));
}

#[test]
fn failed_exec_does_not_create_another_shell() {
    let sandbox = Sandbox::new();
    let path = sandbox.path("broken");
    fs::write(&path, "#!/missing-interpreter-for-mish-tests\n").unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
    let output = sandbox.stdin("./broken\necho exactly-once\n");
    assert!(output.status.success());
    assert!(!output.stderr.is_empty());
    assert_eq!(stdout(&output), "exactly-once\n");
    let output = sandbox.line("no-such-mish-command");
    assert_eq!(output.status.code(), Some(127));
    assert!(!output.stderr.is_empty());
}

#[test]
fn malformed_input_reports_errors_and_next_line_runs() {
    let sandbox = Sandbox::new();
    for source in [
        "echo 'unterminated",
        "echo \"unterminated",
        "echo $(NAME",
        "echo $[echo unfinished",
        "echo trailing\\",
    ] {
        let output = sandbox.line(source);
        assert_eq!(output.status.code(), Some(2), "{source}: {output:?}");
        assert!(!output.stderr.is_empty(), "{source}: {output:?}");
    }
    let output = sandbox.stdin("echo 'unfinished\necho recovered\n");
    assert!(output.status.success());
    assert!(!output.stderr.is_empty());
    assert_eq!(stdout(&output), "recovered\n");
}

#[test]
fn substitution_failure_preserves_stdout_and_reports_stderr() {
    let sandbox = Sandbox::new();
    sandbox.executable(
        "bin/fail-output",
        "printf 'kept\\n'; printf 'diagnostic\\n' >&2; exit 5",
    );
    let output = sandbox.line("echo $[fail-output]");
    assert!(output.status.success());
    assert_eq!(stdout(&output), "kept\n");
    assert_eq!(output.stderr, b"diagnostic\n");
}

#[test]
fn all_thirteen_builtins_are_classified_without_path() {
    let sandbox = Sandbox::new();
    let names = [
        "exit", "which", "echo", "cd", "pwd", "env", "export", "unset", "path", "alias", "unalias",
        "return", "hash",
    ];
    let output = sandbox.line(&format!("which {}", names.join(" ")));
    success(&output);
    for name in names {
        assert!(stdout(&output).contains(&format!("\"{name}\" -> <built-in>\n")));
    }
}

#[test]
fn capture_drains_stderr_as_well_as_stdout() {
    let sandbox = Sandbox::new();
    let payload = "e".repeat(512 * 1024);
    fs::write(sandbox.path("errors"), &payload).unwrap();
    sandbox.executable("bin/large-errors", "/bin/cat errors >&2; printf 'done\\n'");
    let output = sandbox.line("echo $[large-errors]");
    assert!(output.status.success());
    assert_eq!(stdout(&output), "done\n");
    assert_eq!(output.stderr, payload.as_bytes());
}

#[test]
fn signals_and_nonexecutable_files_have_conventional_status() {
    let sandbox = Sandbox::new();
    sandbox.executable("bin/terminated", "kill -TERM $$");
    let output = sandbox.line("terminated");
    assert_eq!(output.status.code(), Some(143));
    fs::write(sandbox.path("not-executable"), "plain file").unwrap();
    let output = sandbox.line("./not-executable");
    assert_eq!(output.status.code(), Some(126));
    assert!(!output.stderr.is_empty());
}

#[test]
fn removing_a_cached_executable_falls_back_and_unset_path_disables_lookup() {
    let sandbox = Sandbox::new();
    sandbox.executable("first/tool", "printf 'first\\n'");
    sandbox.executable("second/tool", "printf 'second\\n'");
    sandbox.executable("bin/remove-first", "exec /bin/rm first/tool");
    let output = sandbox.stdin(&format!(
        "PATH={}:{}:{}\ntool\nremove-first\ntool\nunset PATH\ntool\necho $($)\n",
        quote(&sandbox.path("first")),
        quote(&sandbox.path("second")),
        quote(&sandbox.path("bin")),
    ));
    assert!(output.status.success());
    assert!(!output.stderr.is_empty());
    assert_eq!(stdout(&output), "first\nsecond\n127\n");
}

#[test]
fn foreground_commands_receive_unread_standard_input() {
    let sandbox = Sandbox::new();
    let output = sandbox.stdin("/bin/cat\ninput belongs to cat\nsecond input line\n");
    success(&output);
    assert_eq!(stdout(&output), "input belongs to cat\nsecond input line\n");
}

#[test]
fn excessive_command_substitution_depth_is_a_fatal_parse_error() {
    let sandbox = Sandbox::new();
    let source = format!("echo {}deep{}", "$[echo ".repeat(33), "]".repeat(33));
    let output = sandbox.line(&source);
    assert_eq!(output.status.code(), Some(2));
    assert!(output.stdout.is_empty());
    assert!(String::from_utf8_lossy(&output.stderr).contains("deep"));
}

#[test]
fn assignment_only_line_succeeds_and_exit_uses_previous_status() {
    let sandbox = Sandbox::new();
    let output = sandbox.stdin("return 9\nLOCAL=ok\nexit\n");
    success(&output);
    assert!(output.stdout.is_empty());
    let output = sandbox.stdin("return 9\nexit\n");
    assert_eq!(output.status.code(), Some(9));
}

#[test]
fn capture_exports_unset_and_exit_do_not_change_parent_state() {
    let sandbox = Sandbox::new();
    sandbox.executable("bin/showx", "printf '%s\\n' \"$EXPORTED\"");
    let output = sandbox.stdin(
        "export EXPORTED=outer\necho $[export EXPORTED=inner]\nshowx\necho $[unset EXPORTED]\nshowx\necho $[exit 7]\necho still-running\n",
    );
    success(&output);
    assert_eq!(stdout(&output), "\nouter\n\nouter\n\nstill-running\n");
}
