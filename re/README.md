# Reverse-engineering helpers

Python scripts used to map the game's functions and data (see `../FINDINGS.md`). They work on a memory dump of
the decrypted game image (SteamStub), rebuilt into a PE with `rebuild_pe.py`. No game code or dumps are part of
this repository; the scripts expect the dump at `/home/claude/rofvr/work/rof_dump.exe` (edit `lib.py`).

- `lib.py` image loader / disassembly (capstone), `index.py` builds the cross-reference index, `q.py` queries it
- `match.py`, `findmatch.py`, `side.py`, `vtmatch.py`: match functions of Star Ocean: The Last Hope (same engine)
  to Resonance of Fate by code shape
- `rtti_*.py`, `dispfind.py`, `dispany.py`, `flagfind.py`, `calls.py`, `xref.py`: RTTI and pattern searches
