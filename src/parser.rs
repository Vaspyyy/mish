//! Parsing and expansion for mish's deliberately small command language.
//!
//! Expansion does not introduce argument boundaries: whitespace returned by a
//! variable or command remains part of the word that contains it.

pub trait ExpansionContext {
    fn variable(&self, name: &str) -> Option<String>;
    fn assign(&mut self, name: &str, value: String);
    fn command(&mut self, source: &str) -> Result<String, String>;
}

const MAX_NESTING: usize = 64;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Quote {
    None,
    Single,
    Double,
}

struct Word {
    value: String,
    end: usize,
    quoted: bool,
}

/// Parse a line, applying leading local assignments in source order.
///
/// Only a literal identifier followed by an unquoted `=` is an assignment;
/// expansion results and quoted `=` signs cannot create assignments.
pub fn parse_line(input: &str, ctx: &mut impl ExpansionContext) -> Result<Vec<String>, String> {
    let mut result = Vec::new();
    let mut pos = 0;
    let mut assignments = true;
    while pos < input.len() {
        while let Some(ch) = next_char(input, pos) {
            if !ch.is_whitespace() {
                break;
            }
            pos += ch.len_utf8();
        }
        if pos == input.len() {
            break;
        }
        if assignments {
            if let Some(eq) = assignment_equals(input, pos) {
                let word = expand_word(input, eq + 1, ctx, true, false, 0)?;
                ctx.assign(&input[pos..eq], word.value);
                pos = word.end;
                continue;
            }
        }
        let word = expand_word(input, pos, ctx, true, false, 0)?;
        pos = word.end;
        // An empty quoted word is still a real argv entry. Unquoted missing
        // variables retain mish's behavior of contributing no argument.
        if word.quoted || !word.value.is_empty() {
            assignments = false;
            result.push(word.value);
        }
    }
    Ok(result)
}

/// Expand PS1 without splitting its whitespace into separate words.
pub fn expand_prompt(input: &str, ctx: &mut impl ExpansionContext) -> Result<String, String> {
    Ok(expand_word(input, 0, ctx, false, true, 0)?.value)
}

fn next_char(input: &str, pos: usize) -> Option<char> {
    input.get(pos..)?.chars().next()
}

fn assignment_equals(input: &str, start: usize) -> Option<usize> {
    let bytes = input.as_bytes();
    let first = *bytes.get(start)?;
    if !(first.is_ascii_alphabetic() || first == b'_') {
        return None;
    }
    let mut pos = start + 1;
    while let Some(&byte) = bytes.get(pos) {
        if byte == b'=' {
            return Some(pos);
        }
        if !(byte.is_ascii_alphanumeric() || byte == b'_') {
            return None;
        }
        pos += 1;
    }
    None
}

fn expand_word(
    input: &str,
    start: usize,
    ctx: &mut impl ExpansionContext,
    split: bool,
    prompt: bool,
    depth: usize,
) -> Result<Word, String> {
    if depth >= MAX_NESTING {
        return Err(format!("expansion nesting exceeds {MAX_NESTING}"));
    }
    let mut value = String::new();
    let mut pos = start;
    let mut quote = Quote::None;
    let mut quoted = false;
    let mut word_start = true;
    while let Some(ch) = next_char(input, pos) {
        if quote == Quote::None && split && ch.is_whitespace() {
            break;
        }
        if ch == '\'' && quote != Quote::Double {
            quote = if quote == Quote::Single {
                Quote::None
            } else {
                Quote::Single
            };
            quoted = true;
            word_start = false;
            pos += 1;
            continue;
        }
        if ch == '"' && quote != Quote::Single {
            quote = if quote == Quote::Double {
                Quote::None
            } else {
                Quote::Double
            };
            quoted = true;
            word_start = false;
            pos += 1;
            continue;
        }
        if ch == '\\' && quote != Quote::Single {
            let escaped = next_char(input, pos + 1)
                .ok_or_else(|| format!("unfinished escape at byte {}", pos + 1))?;
            if prompt && escaped == 'e' {
                value.push('\x1b');
                pos += 2;
            } else if quote == Quote::Double && !matches!(escaped, '$' | '"' | '\\' | '\n') {
                // Double quotes preserve backslashes before ordinary letters.
                value.push('\\');
                pos += 1;
            } else {
                if escaped != '\n' {
                    value.push(escaped);
                }
                pos += 1 + escaped.len_utf8();
            }
            if escaped != '\n' {
                word_start = false;
            }
            continue;
        }
        if ch == '$' && quote != Quote::Single {
            if let Some(open @ ('(' | '[')) = next_char(input, pos + 1) {
                let end = substitution_end(input, pos, depth)?;
                let content = &input[pos + 2..end];
                if open == '(' {
                    let name = expand_word(content, 0, ctx, false, false, depth + 1)?.value;
                    if let Some(expansion) = ctx.variable(&name) {
                        value.push_str(&expansion);
                    }
                } else {
                    // The inner command owns interpretation of its syntax.
                    // Removing quotes here would lose its argument boundaries.
                    let expansion = ctx.command(content)?;
                    value.push_str(expansion.trim_end_matches('\n'));
                }
                pos = end + 1;
                word_start = false;
                continue;
            }
        }
        if ch == '~' && quote == Quote::None && word_start {
            if let Some(home) = ctx.variable("HOME") {
                value.push_str(&home);
            } else {
                value.push('~');
            }
        } else {
            value.push(ch);
        }
        word_start = !split && quote == Quote::None && ch.is_whitespace();
        pos += ch.len_utf8();
    }
    if quote != Quote::None {
        let kind = if quote == Quote::Single {
            "single"
        } else {
            "double"
        };
        return Err(format!("unterminated {kind} quote"));
    }
    Ok(Word {
        value,
        end: pos,
        quoted,
    })
}

struct Delimiter {
    open: char,
    close: char,
    quote: Quote,
}

impl Delimiter {
    fn new(open: char) -> Self {
        Self {
            open,
            close: if open == '(' { ')' } else { ']' },
            quote: Quote::None,
        }
    }
}

/// Locate a closer without evaluating its contents. The explicit stack keeps
/// deeply nested or malformed input from overflowing the Rust call stack.
fn substitution_end(input: &str, dollar: usize, depth: usize) -> Result<usize, String> {
    let open = next_char(input, dollar + 1).ok_or("missing substitution opener")?;
    let mut stack = vec![Delimiter::new(open)];
    let mut pos = dollar + 2;
    while let Some(ch) = next_char(input, pos) {
        let frame = stack.last_mut().ok_or("missing substitution delimiter")?;
        if ch == '\'' && frame.quote != Quote::Double {
            frame.quote = if frame.quote == Quote::Single {
                Quote::None
            } else {
                Quote::Single
            };
            pos += 1;
            continue;
        }
        if ch == '"' && frame.quote != Quote::Single {
            frame.quote = if frame.quote == Quote::Double {
                Quote::None
            } else {
                Quote::Double
            };
            pos += 1;
            continue;
        }
        if ch == '\\' && frame.quote != Quote::Single {
            let escaped = next_char(input, pos + 1)
                .ok_or_else(|| format!("unfinished escape at byte {}", pos + 1))?;
            pos += 1 + escaped.len_utf8();
            continue;
        }
        if ch == '$' && frame.quote != Quote::Single {
            if let Some(nested @ ('(' | '[')) = next_char(input, pos + 1) {
                if depth + stack.len() >= MAX_NESTING {
                    return Err(format!("expansion nesting exceeds {MAX_NESTING}"));
                }
                stack.push(Delimiter::new(nested));
                pos += 2;
                continue;
            }
        }
        if frame.quote == Quote::None {
            if ch == frame.close {
                stack.pop();
                if stack.is_empty() {
                    return Ok(pos);
                }
            } else if ch == frame.open {
                if depth + stack.len() >= MAX_NESTING {
                    return Err(format!("expansion nesting exceeds {MAX_NESTING}"));
                }
                stack.push(Delimiter::new(ch));
            }
        }
        pos += ch.len_utf8();
    }
    Err(format!(
        "unterminated ${open} substitution at byte {}",
        dollar + 1
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    #[derive(Default)]
    struct Context {
        vars: BTreeMap<String, String>,
        commands: Vec<String>,
        output: String,
        fail_command: bool,
    }

    impl ExpansionContext for Context {
        fn variable(&self, name: &str) -> Option<String> {
            self.vars.get(name).cloned()
        }
        fn assign(&mut self, name: &str, value: String) {
            self.vars.insert(name.into(), value);
        }
        fn command(&mut self, source: &str) -> Result<String, String> {
            self.commands.push(source.into());
            if self.fail_command {
                Err("command failed".into())
            } else {
                Ok(self.output.clone())
            }
        }
    }

    fn parse(input: &str) -> Vec<String> {
        parse_line(input, &mut Context::default()).unwrap()
    }

    #[test]
    fn mixed_quotes_escaped_whitespace_and_empty_args() {
        assert_eq!(
            parse("\t cmd  'a b' \"c 'd'\" ab\" cd\"ef '' \"\" x\\ y \\\\"),
            ["cmd", "a b", "c 'd'", "ab cdef", "", "", "x y", "\\"]
        );
        assert_eq!(
            parse("echo café\\ monde\u{2003}'東京 大阪'"),
            ["echo", "café monde", "東京 大阪"]
        );
    }

    #[test]
    fn escape_rules_depend_on_quotes() {
        assert_eq!(
            parse(r#"echo '\n' "\n" "\"" "\$[literal]" \$[literal]"#),
            ["echo", r"\n", r"\n", "\"", "$[literal]", "$[literal]"]
        );
        assert_eq!(parse("echo a\\\nb"), ["echo", "ab"]);
    }

    #[test]
    fn assignments_expand_sequentially_and_persist() {
        let mut ctx = Context::default();
        ctx.vars.insert("HOME".into(), "/home/test".into());
        let argv = parse_line(
            "A='one two' B=$(A)/three C=~/dir echo $(B) $(C) A=no",
            &mut ctx,
        )
        .unwrap();
        assert_eq!(argv, ["echo", "one two/three", "/home/test/dir", "A=no"]);
        assert_eq!(ctx.variable("A").as_deref(), Some("one two"));
        assert_eq!(
            parse_line("EMPTY=", &mut ctx).unwrap(),
            Vec::<String>::new()
        );
        assert_eq!(ctx.variable("EMPTY").as_deref(), Some(""));
    }

    #[test]
    fn quotes_and_expansion_cannot_create_assignments() {
        let mut ctx = Context::default();
        ctx.vars.insert("VALUE".into(), "A=value".into());
        assert_eq!(
            parse_line("$(VALUE) B=no", &mut ctx).unwrap(),
            ["A=value", "B=no"]
        );
        assert!(ctx.variable("A").is_none());
        assert_eq!(parse("'A=x' B=no"), ["A=x", "B=no"]);
        assert_eq!(parse("A\\=x B=no"), ["A=x", "B=no"]);
        assert_eq!(parse("9A=x B=no"), ["9A=x", "B=no"]);
    }

    #[test]
    fn variable_syntax_and_nested_names() {
        let mut ctx = Context::default();
        ctx.vars.insert("KEY".into(), "NAME".into());
        ctx.vars.insert("NAME".into(), "a b".into());
        assert_eq!(
            parse_line(
                r#"echo $NAME $(NAME) $($(KEY)) '$(NAME)' "$(ABSENT)" $(ABSENT)"#,
                &mut ctx
            )
            .unwrap(),
            ["echo", "$NAME", "a b", "a b", "$(NAME)", ""]
        );
    }

    #[test]
    fn command_source_retains_quotes_and_nested_substitutions() {
        let mut ctx = Context {
            output: "a b \n\n".into(),
            ..Context::default()
        };
        assert_eq!(
            parse_line(
                r#"echo pre$[echo "a ] b" '$[literal]' $[echo x]]post"#,
                &mut ctx
            )
            .unwrap(),
            ["echo", "prea b post"]
        );
        assert_eq!(ctx.commands, [r#"echo "a ] b" '$[literal]' $[echo x]"#]);
        assert_eq!(
            parse_line(r#"echo "$[echo "x"]""#, &mut ctx).unwrap(),
            ["echo", "a b "]
        );
        assert_eq!(
            parse_line(r"echo $[echo \]]", &mut ctx).unwrap(),
            ["echo", "a b "]
        );
        assert_eq!(ctx.commands.last().unwrap(), r"echo \]");
    }

    #[test]
    fn substitution_failure_is_reported() {
        let mut ctx = Context {
            fail_command: true,
            ..Context::default()
        };
        assert_eq!(
            parse_line("echo $[false]", &mut ctx),
            Err("command failed".into())
        );
    }

    #[test]
    fn tilde_only_expands_at_an_unquoted_word_start() {
        let mut ctx = Context::default();
        ctx.vars.insert("HOME".into(), "/home/test".into());
        assert_eq!(
            parse_line("echo ~/a a~ '~' \"~\" \\~", &mut ctx).unwrap(),
            ["echo", "/home/test/a", "a~", "~", "~", "~"]
        );
        assert_eq!(
            parse_line("echo \\\n~/a", &mut ctx).unwrap(),
            ["echo", "/home/test/a"]
        );
    }

    #[test]
    fn prompt_preserves_whitespace_and_expands_escape() {
        let mut ctx = Context::default();
        ctx.vars.insert("USER".into(), "test".into());
        ctx.vars.insert("HOME".into(), "/home/test".into());
        assert_eq!(
            expand_prompt(r"\e[31m$(USER) ~/x > ", &mut ctx).unwrap(),
            "\x1b[31mtest /home/test/x > "
        );
        assert_eq!(expand_prompt(r"\\e", &mut ctx).unwrap(), r"\e");
    }

    #[test]
    fn malformed_input_and_excessive_depth_fail_without_panicking() {
        for input in [
            "echo '",
            "echo \"",
            "echo \\",
            "echo $(NAME",
            "echo $[echo x",
            "echo $[echo 'x]",
            "echo $[echo $[x]",
        ] {
            assert!(
                parse_line(input, &mut Context::default()).is_err(),
                "{input:?}"
            );
        }
        let deeply_nested = format!("echo {}NAME{}", "$(".repeat(1000), ")".repeat(1000));
        assert!(parse_line(&deeply_nested, &mut Context::default()).is_err());
    }

    #[test]
    fn short_adversarial_fragments_are_safe_for_line_and_prompt_parsing() {
        // Exercise interactions among ASCII syntax and multibyte text, rather
        // than only testing each punctuation mark in isolation.
        let alphabet = [
            'a', 'é', ' ', '\'', '"', '\\', '$', '(', ')', '[', ']', '=', '~',
        ];
        for mut code in 0..alphabet.len().pow(4) {
            let mut input = String::new();
            for _ in 0..4 {
                input.push(alphabet[code % alphabet.len()]);
                code /= alphabet.len();
            }
            let _ = parse_line(&input, &mut Context::default());
            let _ = expand_prompt(&input, &mut Context::default());
        }
    }
}
