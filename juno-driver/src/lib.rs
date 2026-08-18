//! The RTI T2i as a Juno `remote`.
//!
//! This is the T2i-specific half of the integration by definition: the key-code table below is
//! exactly the knowledge Juno must not carry. See `../../docs/JUNO-INTEGRATION.md`.
//!
//! The firmware emits one line per key transition on its USB CDC port
//! (`../../src/main.c`):
//!
//! ```text
//! KEY DOWN 138 Vol + r1 c6
//! KEY UP 138 Vol +
//! ```
//!
//! and this turns those into `remote` notifications. Buttons with a universal meaning become
//! `command`; digits become `digit`; a softkey or a coloured key becomes `custom`, carrying the
//! label the handset prints. Nothing here names a device, which is the whole point — a `volume_up`
//! resolves against whatever the remote's room is currently doing.
//!
//! The code is matched on, not the name. Codes are the stock firmware's own and are stable;
//! the names are for humans and for `custom_labels`.

use driver_sdk::host::{Args, DriverModule, HostCall, Instance};
use driver_sdk::*;
use serde_json::Value;

/// Our own proxy id, matching `[[proxy]] id = 1` in the manifest.
const REMOTE: LocalId = 1;

/// The serial control connection the installer wired us to.
const SERIAL: LocalId = 1;

/// What a T2i key code means to Juno.
enum Key {
    /// A name from `remote`'s closed command vocabulary.
    Command(&'static str),
    /// A number key.
    Digit(u8),
    /// No universal meaning. `index` is a position in the manifest's `custom_labels`, from 1.
    Custom(u32, &'static str),
    /// Deliberately not reported.
    Local,
}

/// Stock RTI key codes 128..=180, from `../../docs/BUTTON-NAMES.md`.
///
/// The joystick maps onto the same d-pad commands as the arrows rather than to `custom`, because a
/// joystick *is* navigation — a rule that wants "go up" should not have to care which of the two
/// the person's thumb found.
fn key(code: u8) -> Option<Key> {
    use Key::*;
    Some(match code {
        128 => Command("exit"),
        129 => Command("mute_toggle"),
        130 => Custom(2, "Soft Lft Cntr"),
        131 => Command("up"),
        132 => Command("left"),
        133 => Command("right"),
        134 => Command("down"),
        135 => Command("select"),
        136 => Custom(1, "Soft Lft"),
        137 => Custom(4, "Soft Rht"),
        138 => Command("volume_up"),
        139 => Command("volume_down"),
        140 => Command("channel_up"),
        141 => Command("channel_down"),
        142 => Command("guide"),
        143 => Command("menu"),
        144 => Command("info"),
        145 => Command("power_off"),
        146 => Command("play"),
        147 => Command("pause"),
        148 => Command("stop"),
        149 => Command("record"),
        150 => Command("scan_reverse"),
        151 => Command("scan_forward"),
        152 => Command("skip_back"),
        153 => Command("skip_forward"),
        154 => Digit(1),
        155 => Digit(2),
        156 => Digit(3),
        157 => Digit(4),
        158 => Digit(5),
        159 => Digit(6),
        160 => Digit(7),
        161 => Digit(8),
        162 => Digit(9),
        163 => Digit(0),
        164 => Custom(10, "-/."),
        165 => Command("enter"),
        166 => Command("up"),      // Scroll Up
        167 => Command("select"),  // Scroll Click
        168 => Command("down"),    // Scroll Down
        169 => Command("left"),    // Scroll Left
        170 => Command("right"),   // Scroll Right
        171 => Command("power_on"),
        172 => Custom(9, "List"),
        173 => Custom(5, "Red"),
        174 => Custom(6, "Green"),
        175 => Custom(7, "Yellow"),
        176 => Custom(8, "Blue"),
        177 => Custom(3, "Soft Rht Cntr"),
        178 => Command("previous_channel"),
        179 => Command("back"),
        180 => Local,              // Backlight — the handset's own function
        _ => return None,
    })
}

#[derive(Default)]
struct T2i;

/// Where a partial line lives between `rx` events. Serial delivers whatever bytes happened to
/// arrive, so a key event is regularly split across two callbacks — dropping the remainder would
/// lose roughly one press in every few.
const PARTIAL: &str = "partial";

/// The key currently down, so an UP knows what it is closing. Some UP lines carry no code.
const DOWN: &str = "down";

/// Whether the key currently down has already crossed into a hold, so the UP that follows is a
/// `release` rather than a second `click`.
const HOLDING: &str = "holding";

fn note_for(k: &Key, action: &str) -> Option<(&'static str, Args)> {
    let mut args = Args::new();
    match k {
        Key::Command(cmd) => {
            args.insert("command".into(), Value::from(*cmd));
            args.insert("action".into(), Value::from(action));
            Some(("command", args))
        }
        Key::Digit(v) => {
            args.insert("value".into(), Value::from(*v));
            args.insert("action".into(), Value::from(action));
            Some(("digit", args))
        }
        Key::Custom(index, label) => {
            args.insert("index".into(), Value::from(*index));
            args.insert("label".into(), Value::from(*label));
            args.insert("action".into(), Value::from(action));
            Some(("custom", args))
        }
        Key::Local => None,
    }
}

/// A human-readable form for the `last_command` state key. A tile that shows nothing reads as a
/// remote that is not working, which is indistinguishable from one that is.
fn describe(k: &Key, action: &str) -> String {
    let what = match k {
        Key::Command(cmd) => (*cmd).to_string(),
        Key::Digit(v) => v.to_string(),
        Key::Custom(_, label) => (*label).to_string(),
        Key::Local => return String::new(),
    };
    if action == "click" {
        what
    } else {
        format!("{what} {action}")
    }
}

/// One key transition from the firmware.
#[derive(PartialEq)]
enum Edge {
    Down,
    /// The key has been down past the firmware's hold threshold. Timed on the remote because a
    /// driver runs in a sandbox with no clock and cannot measure the gap itself.
    Held,
    Up,
}

/// Parse one firmware line: `KEY DOWN|HELD|UP <code> <name...> [rN cM]`.
///
/// The name is skipped. It is there for a human reading the port; the code is what is stable.
fn parse(line: &str) -> Option<(u8, Edge)> {
    let mut f = line.split_whitespace();
    if f.next()? != "KEY" {
        return None;
    }
    let edge = match f.next()? {
        "DOWN" => Edge::Down,
        "HELD" => Edge::Held,
        "UP" => Edge::Up,
        _ => return None,
    };
    Some((f.next()?.parse().ok()?, edge))
}

impl DriverModule for T2i {
    /// A remote takes no commands — it is an input, and `remote` resolves to an empty command set
    /// on purpose. Anything arriving here means a consumer is treating the handset as the thing it
    /// was pointed at, which is worth seeing rather than swallowing.
    fn on_command(
        &self,
        _inst: &mut Instance,
        _proxy: LocalId,
        cmd: &str,
        _args: &Args,
    ) -> Vec<HostCall> {
        vec![HostCall::warn(format!(
            "a remote accepts no commands; got `{cmd}`"
        ))]
    }

    fn on_bind(&self, _inst: &mut Instance) -> Vec<HostCall> {
        // 115200 8N1, matching the firmware's CDC settings. Sent on every bind rather than once:
        // a port that was re-plugged, or wired to a different adapter, comes back at its default.
        let mut args = Args::new();
        args.insert("baud".into(), Value::from(115200));
        args.insert("databits".into(), Value::from(8));
        args.insert("parity".into(), Value::from("none"));
        args.insert("stopbits".into(), Value::from(1));
        args.insert("flow".into(), Value::from("none"));
        vec![HostCall::Invoke {
            control: SERIAL,
            cmd: "configure".into(),
            args,
        }]
    }

    fn on_event(
        &self,
        inst: &mut Instance,
        _control: LocalId,
        note: &str,
        args: &Args,
    ) -> Vec<HostCall> {
        if note != "rx" {
            return Vec::new();
        }

        // `bytes` crosses as a JSON array of numbers, or a string when the provider has already
        // decoded it. Accept both rather than assuming: the contract says bytes, and a provider
        // that hands us text is not wrong enough to drop a keypress over.
        let text = match args.get("data") {
            Some(Value::Array(a)) => {
                let raw: Vec<u8> = a.iter().filter_map(|v| v.as_u64()).map(|n| n as u8).collect();
                String::from_utf8_lossy(&raw).into_owned()
            }
            Some(Value::String(s)) => s.clone(),
            _ => return Vec::new(),
        };

        let mut buf = inst
            .scratch
            .get(PARTIAL)
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        buf.push_str(&text);

        // Keep whatever follows the last newline: it is the start of the next line, not a short one.
        let (complete, rest) = match buf.rfind('\n') {
            Some(i) => (buf[..i].to_string(), buf[i + 1..].to_string()),
            None => (String::new(), buf),
        };
        // A line that never terminates would otherwise grow without bound. Far longer than any
        // real event, so this only ever fires on a port talking to something that is not a T2i.
        inst.scratch.insert(
            PARTIAL.into(),
            Value::from(if rest.len() > 512 { String::new() } else { rest }),
        );

        let mut calls = Vec::new();
        for line in complete.lines() {
            let Some((code, edge)) = parse(line.trim()) else {
                continue;
            };
            let Some(k) = key(code) else {
                calls.push(HostCall::warn(format!("unknown T2i key code {code}")));
                continue;
            };
            if matches!(k, Key::Local) {
                continue;
            }

            // Nothing is emitted on DOWN. A tap must be exactly one `click`, and whether this
            // press is a tap or a hold is not known until either HELD or UP arrives.
            let action = match edge {
                Edge::Down => {
                    inst.scratch.insert(DOWN.into(), Value::from(code));
                    inst.scratch.remove(HOLDING);
                    continue;
                }
                Edge::Held => {
                    inst.scratch.insert(HOLDING.into(), Value::Bool(true));
                    "hold"
                }
                Edge::Up => {
                    // Trust the remembered code: some UP lines carry none. A mismatch means a
                    // press that spanned a restart or a dropped line, and reporting a click for
                    // it would invent an event nobody made.
                    let was = inst.scratch.remove(DOWN).and_then(|v| v.as_u64());
                    let holding = inst.scratch.remove(HOLDING).is_some();
                    if was != Some(code as u64) {
                        continue;
                    }
                    // A hold has to be closed or a consumer that started a ramp never stops it:
                    // a stuck ramp runs the volume to the top with nobody touching it.
                    if holding { "release" } else { "click" }
                }
            };

            if let Some((name, args)) = note_for(&k, action) {
                calls.push(HostCall::notify(REMOTE, name, args));
                calls.push(HostCall::SetState {
                    proxy: REMOTE,
                    key: "last_command".into(),
                    value: Value::from(describe(&k, action)),
                });
            }
        }
        calls
    }
}

export_driver!(T2i);

#[cfg(test)]
mod tests {
    use super::*;

    fn rx(inst: &mut Instance, s: &str) -> Vec<HostCall> {
        let mut args = Args::new();
        args.insert("data".into(), Value::from(s));
        T2i.on_event(inst, SERIAL, "rx", &args)
    }

    fn notes(calls: &[HostCall]) -> Vec<(String, Args)> {
        calls
            .iter()
            .filter_map(|c| match c {
                HostCall::Notify { name, args, .. } => Some((name.clone(), args.clone())),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn every_stock_code_is_accounted_for() {
        // 128..=180 is the whole populated matrix. A gap here is a button that would arrive as a
        // warning and do nothing, which is the failure that is invisible until somebody presses it.
        for code in 128u8..=180 {
            assert!(key(code).is_some(), "code {code} has no mapping");
        }
        assert!(key(127).is_none());
        assert!(key(181).is_none());
    }

    #[test]
    fn custom_indices_match_the_manifest_labels() {
        // `custom_labels` in the manifest is positional, so an index that does not line up puts the
        // wrong name on a rule. Kept in step by checking against the same list.
        let labels: Vec<&str> = "Soft Lft,Soft Lft Cntr,Soft Rht Cntr,Soft Rht,Red,Green,Yellow,Blue,List,-/."
            .split(',')
            .collect();
        let mut seen = vec![false; labels.len()];
        for code in 128u8..=180 {
            if let Some(Key::Custom(index, label)) = key(code) {
                let i = index as usize;
                assert!(i >= 1 && i <= labels.len(), "index {i} out of range");
                assert_eq!(labels[i - 1], label, "index {i} names the wrong label");
                assert!(!seen[i - 1], "index {i} used twice");
                seen[i - 1] = true;
            }
        }
        assert!(seen.iter().all(|&s| s), "a custom_labels entry is unused");
    }

    #[test]
    fn a_hold_opens_and_closes_exactly_once() {
        // The gap this closes: the driver has no clock, so the remote times the hold and says so.
        let mut inst = Instance::default();
        assert!(notes(&rx(&mut inst, "KEY DOWN 138 Vol + r1 c6\n")).is_empty());

        let n = notes(&rx(&mut inst, "KEY HELD 138 Vol +\n"));
        assert_eq!(n.len(), 1);
        assert_eq!(n[0].1["action"], Value::from("hold"));

        // The UP that follows a hold is a release, NOT a second click — a click here would fire
        // the tap rule as well as the ramp.
        let n = notes(&rx(&mut inst, "KEY UP 138 Vol +\n"));
        assert_eq!(n.len(), 1);
        assert_eq!(n[0].1["action"], Value::from("release"));
        assert_eq!(n[0].1["command"], Value::from("volume_up"));
    }

    #[test]
    fn a_press_and_release_is_one_click() {
        let mut inst = Instance::default();
        assert!(notes(&rx(&mut inst, "KEY DOWN 138 Vol + r1 c6\n")).is_empty());

        let calls = rx(&mut inst, "KEY UP 138 Vol +\n");
        let n = notes(&calls);
        assert_eq!(n.len(), 1);
        assert_eq!(n[0].0, "command");
        assert_eq!(n[0].1["command"], Value::from("volume_up"));
        assert_eq!(n[0].1["action"], Value::from("click"));
    }

    #[test]
    fn a_line_split_across_two_reads_still_arrives() {
        // The bug this guards: serial hands over whatever bytes arrived, so events are regularly
        // cut in half. Dropping the remainder loses presses at random.
        let mut inst = Instance::default();
        rx(&mut inst, "KEY DOWN 135 OK r5 c1\nKEY U");
        let calls = rx(&mut inst, "P 135 OK\n");
        let n = notes(&calls);
        assert_eq!(n.len(), 1);
        assert_eq!(n[0].1["command"], Value::from("select"));
    }

    #[test]
    fn digits_carry_a_value() {
        let mut inst = Instance::default();
        rx(&mut inst, "KEY DOWN 163 0 r6 c4\n");
        let n = notes(&rx(&mut inst, "KEY UP 163 0\n"));
        assert_eq!(n[0].0, "digit");
        assert_eq!(n[0].1["value"], Value::from(0));
    }

    #[test]
    fn a_softkey_arrives_as_custom_with_its_printed_label() {
        let mut inst = Instance::default();
        rx(&mut inst, "KEY DOWN 173 Red r3 c2\n");
        let n = notes(&rx(&mut inst, "KEY UP 173 Red\n"));
        assert_eq!(n[0].0, "custom");
        assert_eq!(n[0].1["label"], Value::from("Red"));
        assert_eq!(n[0].1["index"], Value::from(5));
    }

    #[test]
    fn the_backlight_key_is_not_reported() {
        // A local handset function. Emitting it would put a button in the rule editor that does
        // nothing outside the remote's own hand.
        let mut inst = Instance::default();
        rx(&mut inst, "KEY DOWN 180 Backlight r6 c1\n");
        assert!(notes(&rx(&mut inst, "KEY UP 180 Backlight\n")).is_empty());
    }

    #[test]
    fn an_up_with_no_matching_down_invents_nothing() {
        let mut inst = Instance::default();
        assert!(notes(&rx(&mut inst, "KEY UP 138 Vol +\n")).is_empty());
    }

    #[test]
    fn the_joystick_navigates_like_the_arrows() {
        for (scroll, arrow) in [(166u8, 131u8), (168, 134), (169, 132), (170, 133), (167, 135)] {
            let (Some(Key::Command(a)), Some(Key::Command(b))) = (key(scroll), key(arrow)) else {
                panic!("both should be commands");
            };
            assert_eq!(a, b, "scroll {scroll} should match arrow {arrow}");
        }
    }

    #[test]
    fn noise_on_the_port_is_ignored_not_reported() {
        let mut inst = Instance::default();
        let calls = rx(&mut inst, "red\ngreen\nPANEL id=0x4747\nZBX selftest PASS\n");
        assert!(notes(&calls).is_empty());
    }

    #[test]
    fn an_unterminated_line_cannot_grow_without_bound() {
        let mut inst = Instance::default();
        rx(&mut inst, &"x".repeat(600));
        let held = inst.scratch.get(PARTIAL).and_then(Value::as_str).unwrap_or("");
        assert!(held.len() <= 512, "partial buffer grew to {}", held.len());
    }

    #[test]
    fn bytes_arrive_as_an_array_too() {
        // The contract says `bytes`, so a provider handing us a JSON array is the normal case.
        let mut inst = Instance::default();
        let mut args = Args::new();
        args.insert(
            "data".into(),
            Value::from(
                "KEY DOWN 146 Play r4 c2\nKEY UP 146 Play\n"
                    .bytes()
                    .map(Value::from)
                    .collect::<Vec<_>>(),
            ),
        );
        let calls = T2i.on_event(&mut inst, SERIAL, "rx", &args);
        let n = notes(&calls);
        assert_eq!(n.len(), 1);
        assert_eq!(n[0].1["command"], Value::from("play"));
    }
}
