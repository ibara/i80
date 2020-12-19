i80
===
`i80` is an Intel 8080 emulator with built-in CP/M emulation. It allows you to run programs written for CP/M 2.2 and lookalikes to run on a Unix machine. `i80` provides a CPU, 64k of RAM, and a single I/O connection to stdin/stdout on IN/OUT 0.

WIP
---
`i80` is very much a work-in-progress.

Short list of missing features:
* Incomplete AC flag handling (don't use DAA)
* Lots of CP/M BDOS routines
* Filesystem handling
* Bug-free experience

You are welcome to implement any/all of these.

Notes on running
----------------
When running interactive programs, you probably want to run `stty cbreak -echo` before running `i80` so that your terminal operates how CP/M expects. You can reset your terminal after the interactive program exits. There is no need to do this for non-interactive programs.
