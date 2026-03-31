---
name: ceserver-debugger
description: Use this skill when you need to connect to a remote Cheat Engine ceserver, enumerate remote Android processes and modules, and verify addresses, pointer chains, offsets, or struct layouts against live memory.
---

# ceserver Debugger

## Overview

Use this skill for live remote memory verification through Cheat Engine `ceserver`.

It is for tasks like:
- confirming a module base such as `libUE4.so`
- resolving a process name to PID
- reading a pointer chain such as `GWorld -> UWorld -> PhysicsScene`
- validating candidate offsets against real struct contents
- comparing expected offsets with live values when a chain breaks

## Quick Start

1. Confirm TCP reachability first.
   - Prefer `nc -vz <host> <port>`.
2. Use Python and `ceserver-api` for protocol operations.
   - Install to `/tmp/ceserver_api` instead of modifying the repo:
   - `python3 -m pip install --target /tmp/ceserver_api ceserver-api`
3. Connect, enumerate processes, find the target PID, then enumerate modules.
4. Read the module base and apply project offsets against live memory.
5. If a pointer chain fails, dump nearby fields and search for candidates instead of assuming the old offset is still valid.

## Standard Workflow

### 1. Connect and Identify Target

Use a small Python snippet with `CEServerClient`:

```python
import sys
sys.path.insert(0, "/tmp/ceserver_api")
from ceserver_api import CEServerClient

with CEServerClient("192.168.31.235", 52736) as ce:
    procs = ce.enumerate_processes()
    for p in procs:
        if "pubgmhd" in p.name:
            print(p.pid, p.name)
```

Then open the process and get the module base:

```python
with CEServerClient(host, port) as ce:
    ce.pid = target_pid
    ce.open_process()
    for m in ce.enumerate_modules():
        if "libUE4.so" in m.name:
            print(hex(m.base))
```

### 2. Read a Known Chain

Use fixed-width reads:
- `read_uint64` for pointers
- `read_uint32` / `read_uint16` for fields
- `read_bytes` for raw structs or local decoding

Example:

```python
uworld = ce.read_uint64(libue4 + gworld_off)
persistent_level = ce.read_uint64(uworld + persistent_level_off)
```

### 3. Validate Broken Offsets

When an expected field is wrong:
- dump the surrounding object in pointer-sized slots
- separately test promising pointer candidates
- validate candidates by checking downstream fields, not by appearance alone

For example, if `UWorld + PhysicsScene` is wrong:
- dump `UWorld + 0x180 .. 0x500`
- collect pointer-like values
- test each candidate for `sceneCount`, `SceneIndex[]`, or other known `FPhysScene` signatures
- if that still fails, validate global scene maps directly to keep moving

### 4. Fall Back to Global Maps

If the owner object layout changed but a global map is still valid:
- read the global map entries
- identify live scene objects by actor count or downstream object validity
- temporarily route project logic through the verified live object

Use this when:
- `UWorld -> PhysicsScene` changed
- `FPhysScene -> SceneIndex` changed
- but `GPhysXSceneMap` or similar global registries still work

## Practical Rules

- Always confirm the process and module base from live memory before trusting stored offsets.
- Treat lobby and in-match worlds as different states; re-read `GWorld` after scene transitions.
- If a pointer reads as a tiny integer like `0x2`, assume the offset is wrong, not that the object is valid.
- When validating struct candidates, prefer multi-step evidence:
  - field shape looks plausible
  - downstream chain resolves
  - contents are consistent with gameplay state
- Record both the address read from and the value read back.

## Output Expectations

When using this skill, report:
- target process name and PID
- module base used
- each address read and its value
- where the chain broke
- whether the issue is a stale offset, different game state, or a valid fallback path

## References

If you need protocol details, read the installed library source under:
- `/tmp/ceserver_api/ceserver_api/client.py`

If you need to compare against current project offsets, inspect:
- `include/mem_struct.h`
