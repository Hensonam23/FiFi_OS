# FiFi OS Core Requirements (non-negotiable)

FiFi OS is built around these requirements from day one.
They are not optional features added later.

## 1) Gaming-first
- Low latency is a priority (input, audio, scheduling).
- Performance is measured and tracked.
- A "gaming mode" may trade power savings for latency consistency.

## 2) Privacy by default
- No telemetry by default, ever.
- Network features must be explicit and visible.
- No hidden connections or background data collection.

## 3) Security by default
- Least privilege is the design rule.
- Clear boundaries between kernel and userspace.
- Strong defaults: safe configs, minimal attack surface.

## 4) Ease of use
- Clean structure, clean paths, simple commands.
- Docs updated as we go.
- No confusing layouts or random magic steps.

If a design choice conflicts with these, the design choice is wrong.
