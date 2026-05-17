# Code Style Guide

Applies to all Rust, C++, and Objective-C source in this project. Claude should follow these rules exactly when reformatting code. Do not change behavior — style only.

---

## Universal Rules

### 1. Braces on their own line (Allman style)

Opening braces always go on a new line, aligned with the keyword above.

Do:

```cpp
if (condition)
{
    do_thing();
}
```

Don't:

```cpp
if (condition) {
    do_thing();
}
```

### 2. Always brace control flow bodies

Single-statement bodies after `if`, `else`, `while`, `for`, `do`, and `switch`/`match` arms must be wrapped in braces.

Do:

```cpp
if (x > 0)
{
    return x;
}
```

Don't:

```cpp
if (x > 0) return x;

if (x > 0)
    return x;
```

This prevents goto-fail style bugs and keeps diffs clean when statements are added.

---

## Rust

> ⚠️ `rustfmt` does not support Allman braces. Either disable it for this project, use a `rustfmt.skip` attribute on relevant items, or accept that auto-formatting won't apply. Decide before running a style pass.

Functions:

```rust
fn parse_input(input: &str) -> Result<Config, Error>
{
    let trimmed = input.trim();
    Config::from_str(trimmed)
}
```

Match arms — always braced, even for single expressions:

```rust
match value
{
    Some(n) =>
    {
        process(n)
    }
    None =>
    {
        default()
    }
}
```

Other Rust conventions:
- `snake_case` for functions and variables
- `PascalCase` for types and traits
- `SCREAMING_SNAKE_CASE` for constants
- Prefer `?` over explicit `match` for error propagation
- [add project-specific rules here]

---

## C++

Functions:

```cpp
int compute_sum(int a, int b)
{
    return a + b;
}
```

Classes:

```cpp
class Parser
{
public:
    Parser();
    ~Parser();

private:
    int state_;
};
```

Control flow:

```cpp
for (int i = 0; i < n; ++i)
{
    process(i);
}

switch (kind)
{
    case Kind::A:
    {
        handle_a();
        break;
    }
    case Kind::B:
    {
        handle_b();
        break;
    }
}
```

Other C++ conventions:
- `snake_case` for functions and variables, `PascalCase` for classes
- Member variables suffixed with `_` (e.g. `state_`)
- `nullptr` over `NULL`, `using` over `typedef`
- `auto` for iterators and template types; explicit types elsewhere
- [add project-specific rules here]

---

## Objective-C

Methods:

```objc
- (NSString *)formatString:(NSString *)input withPrefix:(NSString *)prefix
{
    if (input == nil)
    {
        return @"";
    }
    return [NSString stringWithFormat:@"%@%@", prefix, input];
}
```

Control flow:

```objc
if ([self isReady])
{
    [self process];
}

for (NSString *item in items)
{
    [self handleItem:item];
}
```

Other Objective-C conventions:
- `camelCase` methods with descriptive parameter labels
- `PascalCase` class names with project prefix (e.g. `XYZParser`)
- Properties: `nonatomic, strong` for objects, `nonatomic, assign` for primitives
- [add project-specific rules here]

---

## Scope & Safety

- Do not change behavior. Style-only edits.
- Do not rename public APIs without a separate task.
- If a bug is spotted, flag it — don't fix it inline with style changes.
- Anything not specified here, match the surrounding code or the language's standard idioms.

---

## Workflow for Claude Code

When reformatting:
1. Process one directory at a time.
2. Run the test suite after each batch.
3. Stop and report if tests fail — don't try to "fix" failures by altering logic.
4. Commit each batch separately with a message like `style: reformat src/auth to STYLE.md`.