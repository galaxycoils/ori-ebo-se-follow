# Ori EBO SE Follow

On-device pet tracking and optional supervised Follow for the original Enabot EBO SE. The current milestone is desktop replay. The exact device installation and motion interfaces for Ori are still unverified.

## Quick start

Install GNU Make, a C++17 compiler, and Python 3. Run these commands from the repository root.

```sh
make test
make replay CASE=retreat
```

The retreat example prints the decisions and finishes with `HOLD → FORWARD → HOLD`. The generated fixtures require no network connection, SDK, private footage, or robot.
